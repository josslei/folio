#pragma once

#include "storage/filesystem.hpp"
#include "storage/index_codec.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace folio::core::storage {

enum class IndexStoreErrorCode {
  Missing,
  Corrupt,
  UnsupportedVersion,
  LimitExceeded,
  InvalidPath,
  IoError,
  CodecError,
  StorageFailure
};

struct IndexStoreError {
  IndexStoreErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::optional<StorageError> storage_error;
  std::optional<index_codec::IndexCodecError> codec_error;
};

enum class SaveOutcome { Saved, SkippedClean };

template <class T>
using IndexStoreResult = std::variant<T, IndexStoreError>;

// Local plaintext cache at state_root/index/normal.msgpack. The caller owns
// catalog lifecycle and clears its dirty flag only after SaveOutcome::Saved.
// No filesystem access occurs in the constructor or for a clean save.
class NormalIndexStore {
 public:
  explicit NormalIndexStore(StoragePaths paths);

  [[nodiscard]] IndexStoreResult<MetadataCatalog> load() const;
  [[nodiscard]] IndexStoreResult<SaveOutcome> save(const MetadataCatalog& catalog) const;

 private:
  StoragePaths paths_;
};

}  // namespace folio::core::storage
