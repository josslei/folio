#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace folio::io {
enum class FileErrorCode { InvalidPath, IoError, DurabilityFailure, RecoveryRequired };
struct FileError {
  FileErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
  bool replacement_completed = false;
  // Required destination publication sync succeeded; later artifact cleanup may fail.
  bool durability_confirmed = false;
};
enum class ReadErrorCode { IoError, LimitExceeded };
struct ReadError {
  ReadErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
};
// Owns opaque bytes; std::string supplies storage only, with no text decoding.
// The caller supplies confinement/symlink policy and a maximum byte count.
std::variant<std::string, ReadError> read_file(const std::filesystem::path& path,
                                            std::size_t max_bytes);
// Contents are opaque bytes: no text encoding, parsing, or encryption occurs
// here. Normal storage can pass serialized text, and Vault can pass an already
// encrypted binary envelope to these same operations.
// The parent directory must already exist. Create fails if the destination
// exists; replace fails if it does not. Both reject directories and symlinks.
// Temporary names are .<destination-filename>.tmp-<random-uuid>. A pending temp
// blocks subsequent writes to that entry. Cooperating writers lock the parent
// directory; a busy directory causes an error. External writers must be excluded.
// On POSIX, the temporary file and its directory are synced before publication,
// then the directory is synced again. Create publishes by linking the staged
// file; replace consumes the temporary name during rename. If a sync fails,
// the operation attempts to remove its own temporary file before returning.
// Cleanup can itself fail, and a crash can leave a temporary file for recovery.
// A failure after publication requires caller reconciliation before further
// writes; recovery is outside this API. A successful rename is not rolled back
// by a failed sync.
[[nodiscard]] std::variant<std::monostate, FileError> create_file_atomic(
    const std::filesystem::path& path, std::span<const std::byte> contents);
[[nodiscard]] std::variant<std::monostate, FileError> replace_file_atomic(
    const std::filesystem::path& path, std::span<const std::byte> contents);

// Lifecycle-only durability helpers. Absolute paths without symlink components
// are required; callers exclude concurrent external changes. Directory creation
// syncs every ancestor link on POSIX, including existing links (safe retry after
// a prior failed sync). File sync/removal reject non-regular files and symlinks.
// Removal syncs the parent even if the file is already absent, permitting retry
// after unlink succeeded but its directory sync failed. replacement_completed
// means the unlink happened, not that its durability was confirmed.
// Windows flushes file contents; directory creation/deletion metadata has no
// portable flush guarantee through these APIs. See architecture durability notes.
[[nodiscard]] std::variant<std::monostate, FileError>
create_directories_durable(const std::filesystem::path& path);
// Presence-only files need no staged payload: exclusive creation avoids orphan
// staging files blocking the next lifecycle attempt. Existing files are never
// overwritten. A failed sync leaves the published empty file conservatively.
[[nodiscard]] std::variant<std::monostate, FileError>
create_empty_file_durable(const std::filesystem::path& path);
[[nodiscard]] std::variant<std::monostate, FileError>
sync_regular_file(const std::filesystem::path& path);
[[nodiscard]] std::variant<std::monostate, FileError>
remove_file_durable(const std::filesystem::path& path);


inline std::variant<std::monostate, FileError> create_file_atomic(
    const std::filesystem::path& path, std::string_view contents) {
  return create_file_atomic(path, std::as_bytes(std::span{contents.data(), contents.size()}));
}
inline std::variant<std::monostate, FileError> replace_file_atomic(
    const std::filesystem::path& path, std::string_view contents) {
  return replace_file_atomic(path, std::as_bytes(std::span{contents.data(), contents.size()}));
}
}  // namespace folio::io
