#pragma once

#include "core/internal/result.hpp"

#include "storage/filesystem.hpp"
#include "storage/index_codec.hpp"
#include "storage/normal_repository.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <utility>
#include <vector>

namespace folio::core::storage {

enum class CoordinatorErrorCode {
  Missing, Corrupt, UnsupportedVersion, LimitExceeded, InvalidPath,
  IoError, CodecError, StorageFailure, Lifecycle, Unavailable
};

struct CoordinatorError {
  CoordinatorErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::optional<StorageError> storage_error;
  std::optional<index_codec::IndexCodecError> codec_error;
  std::optional<BinaryReadError> binary_error;
};

// Successful initialization: no index_error means the persisted Index loaded.
// Otherwise it records why a full authoritative rebuild was needed and succeeded.
struct InitializationOutcome {
  std::optional<CoordinatorError> index_error;
  std::vector<ExcludedObject> diagnostics;
};

enum class CatalogStatus { Unloaded, Ready, Unavailable, Inconsistent };
enum class SaveOutcome { Saved, SkippedPersisted };

// Coordinates an owner-supplied Catalog without owning it. The Catalog must
// outlive this coordinator and its callbacks. Startup load is a single attempt;
// saving once at shutdown is caller policy and failed saves may be retried.
// Failed startup cannot be turned into save permission by editing the Catalog.
class CatalogCoordinator {
 public:
  CatalogCoordinator(StoragePaths paths, MetadataCatalog& catalog);
  CatalogCoordinator(const CatalogCoordinator&) = delete;
  CatalogCoordinator& operator=(const CatalogCoordinator&) = delete;
  CatalogCoordinator(CatalogCoordinator&&) = delete;
  CatalogCoordinator& operator=(CatalogCoordinator&&) = delete;

  // One-time startup policy: load the Index, rebuilding from Normal files only
  // when the cache is missing, corrupt, unsupported, or over a resource limit.
  [[nodiscard]] ::folio::core::internal::Result<InitializationOutcome, CoordinatorError>
  startup_load();
  // Explicit full replacement from authoritative Normal files, not a targeted
  // correction of individual cached records.
  [[nodiscard]] ::folio::core::internal::Result<std::vector<ExcludedObject>, CoordinatorError>
  refresh_all();
  [[nodiscard]] ::folio::core::internal::Result<SaveOutcome, CoordinatorError> shutdown_save();
  [[nodiscard]] CatalogStatus status() const noexcept;
  // Created once by the constructor. Accessors borrow the stored callbacks;
  // use them by const reference and keep this coordinator alive during use.
  // Creation inserts explicitly; update also covers moves and Directory rename.
  [[nodiscard]] const PersistedCallback<LogMetadata>& log_creation_callback() const noexcept {
    return log_creation_callback_;
  }
  [[nodiscard]] const PersistedCallback<LogMetadata>& log_update_callback() const noexcept {
    return log_update_callback_;
  }
  [[nodiscard]] const PersistedCallback<MemoMetadata>& memo_creation_callback() const noexcept {
    return memo_creation_callback_;
  }
  [[nodiscard]] const PersistedCallback<MemoMetadata>& memo_update_callback() const noexcept {
    return memo_update_callback_;
  }
  [[nodiscard]] const PersistedCallback<TodoMetadata>& todo_creation_callback() const noexcept {
    return todo_creation_callback_;
  }
  [[nodiscard]] const PersistedCallback<TodoMetadata>& todo_update_callback() const noexcept {
    return todo_update_callback_;
  }
  [[nodiscard]] const PersistedCallback<DirectoryMetadata>& directory_creation_callback() const noexcept {
    return directory_creation_callback_;
  }
  [[nodiscard]] const PersistedCallback<DirectoryMetadata>& directory_update_callback() const noexcept {
    return directory_update_callback_;
  }

 private:
  // Used only during construction to compose Catalog updates and consistency
  // handling. Errors before invocation still require caller reconciliation.
  template <class Update>
  auto wrap_catalog_update(Update update) {
    return [this, update = std::move(update)](const auto& metadata) mutable
        -> ::folio::core::internal::Result<std::monostate, StorageError> {
      try {
        auto result = update(metadata);
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
  [[nodiscard]] ::folio::core::internal::Result<std::vector<ExcludedObject>, CoordinatorError> rebuild();
  StoragePaths paths_;
  MetadataCatalog& catalog_;
  CatalogStatus status_ = CatalogStatus::Unloaded;
  bool load_attempted_ = false;
  const PersistedCallback<LogMetadata> log_creation_callback_;
  const PersistedCallback<LogMetadata> log_update_callback_;
  const PersistedCallback<MemoMetadata> memo_creation_callback_;
  const PersistedCallback<MemoMetadata> memo_update_callback_;
  const PersistedCallback<TodoMetadata> todo_creation_callback_;
  const PersistedCallback<TodoMetadata> todo_update_callback_;
  const PersistedCallback<DirectoryMetadata> directory_creation_callback_;
  const PersistedCallback<DirectoryMetadata> directory_update_callback_;
};

}  // namespace folio::core::storage
