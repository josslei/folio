#pragma once

#include "folio/storage/filesystem.hpp"

namespace folio::storage::internal {

// Internal syscall seam for deterministic POSIX durability failure tests.
// Windows does not use the sync callback.
using SyncFile = bool (*)(int);
Result<std::monostate> write_file_atomic(
    const std::filesystem::path& path, std::string_view contents,
    bool create_only, SyncFile sync);

}  // namespace folio::storage::internal
