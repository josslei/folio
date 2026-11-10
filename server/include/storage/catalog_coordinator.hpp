#pragma once

#include <folio/core/result.hpp>

#include <folio/io/filesystem.hpp>
#include "storage/common.hpp"
#include "storage/index_codec.hpp"
#include "storage/normal_repository.hpp"

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
/// under a short mutex. Lifecycle I/O must wait for accepted work to drain. Index persistence is
/// not Record persistence.
class CatalogCoordinator {
public:
  CatalogCoordinator(StoragePaths paths, MetadataCatalog& catalog);
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
  // correction of individual cached records.
  [[nodiscard]] ::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError> refresh_all();
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
  /// Safe during managed mutations. Startup/Refresh/finalization require drained work.
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

  // Borrowed, read-only UUID check_creation for all four creation operations.
  // Rejects unavailable/inconsistent Catalogs; does not reserve an ID.
  [[nodiscard]] const CreationCheck& creation_check() const noexcept { return creation_check_; }

private:
  // Compose metadata updates and failure handling once. Catalog-update failure
  // marks inconsistency here because Repository never recursively calls a failed
  // callback. Failure notifications do not modify metadata or unpersisted state.
  template <class Metadata, class Update>
  MutationResultCallback<Metadata> wrap_catalog_update(Update update) {
    return [this, update = std::move(update)](const MutationNotification<Metadata>& notification)
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
      try {
        auto result = update(std::get<std::reference_wrapper<const Metadata>>(notification).get());
        if (std::holds_alternative<StorageError>(result)) mark_needs_refresh();
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
  StoragePaths paths_;
  MetadataCatalog& catalog_;
  /// Protects managed Catalog publication and read_state capture; never held for
  /// Repository I/O. Single writer owns lifecycle fields; startup/Refresh/finalization
  /// run only with execution drained. Status is atomic for independent availability checks.
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
