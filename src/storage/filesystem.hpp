#pragma once

#include "core/internal/result.hpp"

#include "folio/core/directory.hpp"
#include "folio/core/types.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace folio::core::storage {

struct StoragePaths {
  std::filesystem::path normal_root;
  std::filesystem::path vault_root;
  std::filesystem::path state_root;
  std::filesystem::path cache_root;
};

enum class EntryType { Log, Memo, Todo };
using core::DirectoryKind;

enum class StorageErrorCode {
  InvalidPath,
  InvalidTimestamp,
  InvalidEntryType,
  IoError,
  DurabilityFailure,
  RecoveryRequired,
  CodecError,
  IdentityMismatch,
  DuplicateId,
  StaleLocator,
  InvalidMetadata,
  NotFound,
  ReferencedDirectory
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
// when checking that data roots and the state root do not overlap.
[[nodiscard]] ::folio::core::internal::Result<StoragePaths, StorageError> validate_storage_paths(const StoragePaths& paths);

[[nodiscard]] ::folio::core::internal::Result<std::string, StorageError> normal_entry_filename(
    EntryType type, const core::Id& id, core::Timestamp created);

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
