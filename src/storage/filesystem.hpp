#pragma once

#include "core/internal/result.hpp"

#include "folio/core/directory.hpp"
#include "folio/core/types.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace folio::core::storage {

enum class BinaryReadErrorCode { IoError, LimitExceeded };

struct BinaryReadError {
  BinaryReadErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
};

// Reads opaque bytes, including embedded NUL. The caller supplies the cap and
// owns path confinement and symlink policy.
[[nodiscard]] ::folio::core::internal::Result<std::string, BinaryReadError> read_binary_file(
    const std::filesystem::path& path, std::size_t max_bytes);

// Effective application-owned directories, supplied explicitly for now. Platform
// default resolution belongs to future configuration setup. No directories are
// created by constructing or validating these values. Runtime may be unavailable
// (including platforms whose IPC transport needs no filesystem directory).
struct ApplicationPaths {
  std::filesystem::path config;
  std::filesystem::path data;
  std::filesystem::path state;
  std::filesystem::path cache;
  std::optional<std::filesystem::path> runtime;
};

struct StoragePaths {
  // User-configurable authoritative locations, independent of application paths.
  std::filesystem::path normal_root;
  std::filesystem::path vault_root;
  ApplicationPaths application;
};

enum class RecordType { Log, Memo, Todo };
using core::DirectoryKind;

enum class StorageErrorCode {
  InvalidPath,
  InvalidTimestamp,
  InvalidRecordType,
  IoError,
  DurabilityFailure,
  RecoveryRequired,
  CodecError,
  IdentityMismatch,
  DuplicateId,
  StaleLocator,
  InvalidMetadata,
  NotFound,
  ReferencedDirectory,
  // Authoritative persistence succeeded, but metadata projection/callback failed.
  PostPersistFailure
};

struct StorageError {
  StorageErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
  // True if creation or replacement was published, even when a later sync or
  // cleanup failed. This does not by itself confirm crash durability.
  bool replacement_completed = false;
};

// Validation does not create directories. Existing symlink prefixes are resolved
// when checking authoritative roots against each other and application paths.
// Application categories may share or nest directories; authoritative storage
// must remain disjoint from all application-managed directories.
[[nodiscard]] ::folio::core::internal::Result<StoragePaths, StorageError> validate_storage_paths(const StoragePaths& paths);

[[nodiscard]] ::folio::core::internal::Result<std::string, StorageError> normal_record_filename(
    RecordType type, const core::Id& id, core::Timestamp created);

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
[[nodiscard]] ::folio::core::internal::Result<std::monostate, StorageError> create_file_atomic(
    const std::filesystem::path& path, std::string_view contents);
[[nodiscard]] ::folio::core::internal::Result<std::monostate, StorageError> replace_file_atomic(
    const std::filesystem::path& path, std::string_view contents);

}  // namespace folio::core::storage
