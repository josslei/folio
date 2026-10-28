#pragma once

#include "storage/filesystem.hpp"

namespace folio::core::storage::internal {

// Internal syscall seam for deterministic POSIX durability failure tests.
// Windows does not use the sync callback.
using SyncFile = bool (*)(int);
::folio::core::internal::Result<std::monostate, StorageError> write_file_atomic(
    const std::filesystem::path& path, std::string_view contents,
    bool create_only, SyncFile sync);

}  // namespace folio::core::storage::internal
