#pragma once

#include "storage/binary_io.hpp"
#include "storage/filesystem.hpp"
#include "storage/index_codec.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

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

template <class T>
using CoordinatorResult = std::variant<T, CoordinatorError>;

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

  [[nodiscard]] CoordinatorResult<std::monostate> startup_load();
  [[nodiscard]] CoordinatorResult<SaveOutcome> shutdown_save();
  [[nodiscard]] const MetadataCatalog& catalog() const noexcept;
  [[nodiscard]] MetadataCatalog& catalog() noexcept;
  [[nodiscard]] CatalogStatus status() const noexcept;

 private:
  StoragePaths paths_;
  MetadataCatalog catalog_;
  CatalogStatus status_ = CatalogStatus::Unloaded;
  bool load_attempted_ = false;
};

}  // namespace folio::core::storage
