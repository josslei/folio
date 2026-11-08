#include "storage/common.hpp"
#include <folio/config/configuration.hpp>

#include <utility>

namespace folio::core::storage {

StorageResult<StoragePaths> validate_storage_paths(const StoragePaths& paths) {
  auto result = config::validate_effective_paths(paths);
  if (auto* failure = std::get_if<config::ConfigurationError>(&result)) {
    return io::FileError{failure->code == config::ConfigurationErrorCode::IoError
                             ? io::FileErrorCode::IoError
                             : io::FileErrorCode::InvalidPath,
                         failure->file, failure->message, failure->system_error};
  }
  return std::get<StoragePaths>(std::move(result));
}

} // namespace folio::core::storage
