#pragma once

#include "core/internal/result.hpp"

#include "storage/filesystem.hpp"
#include "storage/index_codec.hpp"
#include "storage/normal_repository.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
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

enum class CatalogStatus { Unloaded, Ready, Unavailable };
enum class SaveOutcome { Saved, SkippedPersisted };

// Owns one in-memory catalog. Startup load is a single attempt. Saving once at
// shutdown is caller policy; failed saves may be retried. Failed startup cannot
// be turned into save permission by editing the catalog.
class CatalogCoordinator {
 public:
  explicit CatalogCoordinator(StoragePaths paths);
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
  [[nodiscard]] const MetadataCatalog& catalog() const noexcept;
  [[nodiscard]] MetadataCatalog& catalog() noexcept;
  [[nodiscard]] CatalogStatus status() const noexcept;

 private:
  // Explicit full rebuild/Refresh from authoritative Normal files. Failure
  // preserves catalog and readiness; success is Ready and unpersisted, with no
  // Index I/O, and prevents a later stale startup load from replacing it.
  [[nodiscard]] ::folio::core::internal::Result<std::vector<ExcludedObject>, CoordinatorError> rebuild();
  StoragePaths paths_;
  MetadataCatalog catalog_;
  CatalogStatus status_ = CatalogStatus::Unloaded;
  bool load_attempted_ = false;
};

}  // namespace folio::core::storage
