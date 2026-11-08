#pragma once

#include <folio/io/filesystem.hpp>

namespace folio::io::internal {

// Internal syscall seam for deterministic POSIX durability failure tests.
// Windows does not use the sync callback.
using SyncFile = bool (*)(int);
std::variant<std::monostate, FileError> write_file_atomic(const std::filesystem::path& path,
                                                          std::span<const std::byte> contents,
                                                          bool create_only, SyncFile sync);

} // namespace folio::io::internal
