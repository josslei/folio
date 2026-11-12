#pragma once

#include <folio/core/result.hpp>

#include <folio/io/filesystem.hpp>
#include "storage/common.hpp"
#include "storage/index_codec.hpp"
#include "storage/normal_repository.hpp"

#include <concepts>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <utility>
#include <vector>

namespace folio::core::storage {

enum class CoordinatorErrorCode {
  Missing,
  Corrupt,
  UnsupportedVersion,
  LimitExceeded,
  InvalidPath,
  IoError,
  CodecError,
  StorageFailure,
  Lifecycle,
  Unavailable
};

struct CoordinatorError {
  CoordinatorErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::optional<StorageError> storage_error;
  std::optional<index_codec::IndexCodecError> codec_error;
  std::optional<io::ReadError> binary_error;
  std::optional<io::FileError> file_error;
};

// Successful initialization: without a previous Marker, no index_error means
// the persisted Index loaded. A previous Marker bypasses the Index entirely.
// index_error records a rejected Cache only, not Marker-triggered recovery.
struct InitializationOutcome {
  std::optional<CoordinatorError> index_error;
  std::vector<ExcludedObject> diagnostics;
  bool previous_session_incomplete = false;
};

enum class CatalogStatus { Unloaded, Ready, Unavailable, Inconsistent, Stopped };
enum class SaveOutcome { Saved, SkippedPersisted };

/// Coordinates an owner-supplied Catalog without owning it. The Catalog must
/// outlive this coordinator and its callbacks. Startup load is a single attempt;
/// saving once at shutdown is caller policy and failed saves may be retried.
/// Failed startup cannot be turned into save permission by editing the Catalog.
/// One mutation worker serializes callbacks; read_state() captures roots/status
/// under a short mutex. Refresh excludes Mutations; startup/finalization must wait
/// for accepted work to drain. Index persistence is
/// not Record persistence.
class CatalogCoordinator {
public:
  CatalogCoordinator(StoragePaths paths, MetadataCatalog& catalog);
  /// Sanitized publication facts for the Application notification adapter. Invoked
  /// under publication_mutex_; observer must not reenter Storage or perform I/O.
  struct RecordChange {
    core::Id id;
    core::RecordType type;
    std::optional<core::Id> directory_id;
    // Absent on creation; otherwise the previous Root or Directory.
    std::optional<std::variant<core::RecordType, core::Id>> previous_location;
  };
  struct DirectoryChange {
    core::Id id;
    core::DirectoryKind kind;
  };
  struct Change {
    std::variant<RecordChange, DirectoryChange, CatalogStatus, std::monostate> value;
    bool uncertain = false;
    bool content = true;
  };
  /// Install/remove only with execution drained. Observer exceptions call failed;
  /// delivery failure never changes persistence/Catalog results. Neither callback may block.
  void set_change_observer(std::function<void(const Change&)> observer,
                           std::function<void()> failed = {}) {
    observer_ = std::move(observer);
    observer_failed_ = std::move(failed);
  }
  CatalogCoordinator(const CatalogCoordinator&) = delete;
  CatalogCoordinator& operator=(const CatalogCoordinator&) = delete;
  CatalogCoordinator(CatalogCoordinator&&) = delete;
  CatalogCoordinator& operator=(CatalogCoordinator&&) = delete;

  // One-time startup policy: load the Index, rebuilding from Normal files only
  // when the cache is missing, corrupt, unsupported, or over a resource limit,
  // or a previous durable Session Marker exists. Ready requires a durable Marker.
  // Uses application.cache/index/normal.msgpack; former State indexes are ignored
  // and are neither migrated nor removed. Construction performs no Index I/O.
  [[nodiscard]] ::folio::core::Result<InitializationOutcome, CoordinatorError> startup_load();
  // Explicit full replacement from authoritative Normal files, not a targeted
  // correction of individual cached records. Caller excludes Mutations through
  // completion; Access captures/publication share a short mutex.
  [[nodiscard]] ::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError> refresh_all();
  /// Publish a privately validated maintenance candidate. Caller excludes Mutations
  /// through completion; concurrent Access retains immutable views. Scoped publication
  /// preserves readiness and never clears global Inconsistent state.
  [[nodiscard]] ::folio::core::Result<std::monostate, CoordinatorError>
  publish_scoped_refresh(MetadataCatalog candidate, std::function<void()> published = {});
  [[nodiscard]] ::folio::core::Result<SaveOutcome, CoordinatorError> shutdown_save();
  // Backend shutdown entry point: stop work first, then call finish_session().
  // Saves if needed, then durably removes the Marker; failures are retryable.
  // shutdown_save() alone leaves the Marker in place and does not end a session.
  // No authoritative mutations may follow successful finalization.
  [[nodiscard]] ::folio::core::Result<SaveOutcome, CoordinatorError> finish_session();
  [[nodiscard]] CatalogStatus status() const noexcept;
  struct ReadState {
    CatalogStatus status;
    MetadataCatalogView view;
  };
  /// Capture status and immutable roots under the same short publication boundary.
  /// Safe during managed mutations. Startup/finalization require drained work; Refresh excludes
  /// Mutations.
  [[nodiscard]] ReadState read_state() const;
  // Created once by the constructor. Accessors borrow the stored callbacks;
  // use them by const reference and keep this coordinator alive during use.
  // Creation inserts explicitly; update also covers moves and Directory rename.
  [[nodiscard]] const MutationResultCallback<LogMetadata>& log_creation_callback() const noexcept {
    return log_creation_callback_;
  }
  [[nodiscard]] const MutationResultCallback<LogMetadata>& log_update_callback() const noexcept {
    return log_update_callback_;
  }
  [[nodiscard]] const MutationResultCallback<MemoMetadata>&
  memo_creation_callback() const noexcept {
    return memo_creation_callback_;
  }
  [[nodiscard]] const MutationResultCallback<MemoMetadata>& memo_update_callback() const noexcept {
    return memo_update_callback_;
  }
  [[nodiscard]] const MutationResultCallback<TodoMetadata>&
  todo_creation_callback() const noexcept {
    return todo_creation_callback_;
  }
  [[nodiscard]] const MutationResultCallback<TodoMetadata>& todo_update_callback() const noexcept {
    return todo_update_callback_;
  }
  [[nodiscard]] const MutationResultCallback<DirectoryMetadata>&
  directory_creation_callback() const noexcept {
    return directory_creation_callback_;
  }
  [[nodiscard]] const MutationResultCallback<DirectoryMetadata>&
  directory_update_callback() const noexcept {
    return directory_update_callback_;
  }

  /// Application supplies the known body-change fact after reconstructing current
  /// authoritative fields. Publication and notification remain one callback boundary.
  template <class Metadata>
    requires(std::same_as<Metadata, LogMetadata> || std::same_as<Metadata, MemoMetadata> ||
             std::same_as<Metadata, TodoMetadata>)
  MutationResultCallback<Metadata> update_callback(bool content_changed) {
    return wrap_catalog_update<Metadata>(
        [this](const Metadata& value) { return catalog_.update_record(value); }, content_changed);
  }

  // Borrowed, read-only UUID check_creation for all four creation operations.
  // Rejects unavailable/inconsistent Catalogs; does not reserve an ID.
  [[nodiscard]] const CreationCheck& creation_check() const noexcept { return creation_check_; }

private:
  /// PushNotification construction/delivery failures never change an authoritative result.
  template <class Build> void notify_observer(Build&& build) noexcept {
    if (!observer_) return;
    try {
      observer_(build());
    } catch (...) {
      notification_failed();
    }
  }
  void notification_failed() noexcept {
    try {
      if (observer_failed_) observer_failed_();
    } catch (...) {
    }
  }
  // Compose metadata updates and failure handling once. Catalog-update failure
  // marks inconsistency here because Repository never recursively calls a failed
  // callback. Failure notifications do not modify metadata or unpersisted state.
  template <class Metadata, class Update>
  MutationResultCallback<Metadata> wrap_catalog_update(Update update, bool content = true) {
    return [this, update = std::move(update),
            content](const MutationNotification<Metadata>& notification)
               -> ::folio::core::Result<std::monostate, StorageError> {
      std::lock_guard lock(publication_mutex_);
      if (const auto* failure =
              std::get_if<std::reference_wrapper<const RepositoryMutationError>>(&notification)) {
        if (failure->get().stage != MutationFailureStage::BeforeAuthoritativeChange)
          mark_needs_refresh();
        return std::monostate{};
      }
      if (!session_active_)
        return StorageError{StorageErrorCode::CatalogUnavailable,
                            paths_.normal_root,
                            "Catalog session is not Ready",
                            {},
                            false};
      const auto& incoming = std::get<std::reference_wrapper<const Metadata>>(notification).get();
      try {
        std::optional<std::variant<core::RecordType, core::Id>> previous;
        if constexpr (!std::is_same_v<Metadata, DirectoryMetadata>) {
          if (observer_) {
            try {
              if (auto old = catalog_.read_view().find_record(incoming.metadata.id))
                previous = std::visit(
                    [](const auto& item) -> std::variant<core::RecordType, core::Id> {
                      using M = std::decay_t<decltype(item.metadata)>;
                      if constexpr (std::is_same_v<M, core::LogMetadata>)
                        return core::RecordType::Log;
                      else {
                        if (item.metadata.directory_id) return *item.metadata.directory_id;
                        return std::is_same_v<M, core::MemoMetadata> ? core::RecordType::Memo
                                                                     : core::RecordType::Todo;
                      }
                    },
                    *old);
            } catch (...) {
              notification_failed();
            }
          }
        }
        auto result = update(std::get<std::reference_wrapper<const Metadata>>(notification).get());
        if (std::holds_alternative<StorageError>(result)) mark_needs_refresh();
        notify_observer([&] {
          if constexpr (std::is_same_v<Metadata, DirectoryMetadata>) {
            return Change{DirectoryChange{incoming.metadata.id, incoming.metadata.kind},
                          std::holds_alternative<StorageError>(result), content};
          } else {
            constexpr auto type = std::is_same_v<Metadata, LogMetadata>    ? core::RecordType::Log
                                  : std::is_same_v<Metadata, MemoMetadata> ? core::RecordType::Memo
                                                                           : core::RecordType::Todo;
            std::optional<core::Id> directory;
            if constexpr (!std::is_same_v<Metadata, LogMetadata>)
              directory = incoming.metadata.directory_id;
            return Change{RecordChange{incoming.metadata.id, type, directory, previous},
                          std::holds_alternative<StorageError>(result), content};
          }
        });
        return result;
      } catch (...) {
        mark_needs_refresh();
        throw;
      }
    };
  }
  // Does not change Catalog records or unpersisted state. Only successful
  // authoritative reconciliation restores Ready; shutdown refuses Inconsistent.
  void mark_needs_refresh() noexcept;
  // Explicit full rebuild/Refresh from authoritative Normal files. Failure
  // preserves catalog and readiness; success is Ready and unpersisted, with no
  // Index I/O, and prevents a later stale startup load from replacing it.
  [[nodiscard]] ::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError> rebuild();
  [[nodiscard]] ::folio::core::Result<bool, CoordinatorError> previous_marker() const;
  [[nodiscard]] ::folio::core::Result<std::monostate, CoordinatorError> establish_marker();
  std::function<void(const Change&)> observer_;
  std::function<void()> observer_failed_;
  StoragePaths paths_;
  MetadataCatalog& catalog_;
  /// Protects managed Catalog publication and read_state capture; never held for
  /// Repository I/O. Single writer owns lifecycle fields; startup/finalization
  /// run only with execution drained; Refresh excludes Mutations. Status is atomic for independent
  /// availability checks.
  mutable std::mutex publication_mutex_;
  std::atomic<CatalogStatus> status_{CatalogStatus::Unloaded};
  bool load_attempted_ = false;
  bool session_active_ = false;
  bool marker_cleanup_pending_ = false;
  const CreationCheck creation_check_;
  const MutationResultCallback<LogMetadata> log_creation_callback_;
  const MutationResultCallback<LogMetadata> log_update_callback_;
  const MutationResultCallback<MemoMetadata> memo_creation_callback_;
  const MutationResultCallback<MemoMetadata> memo_update_callback_;
  const MutationResultCallback<TodoMetadata> todo_creation_callback_;
  const MutationResultCallback<TodoMetadata> todo_update_callback_;
  const MutationResultCallback<DirectoryMetadata> directory_creation_callback_;
  const MutationResultCallback<DirectoryMetadata> directory_update_callback_;
};

} // namespace folio::core::storage
