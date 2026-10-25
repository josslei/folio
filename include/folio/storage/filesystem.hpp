#pragma once

#include "folio/core/types.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace folio::storage {

struct StoragePaths {
  std::filesystem::path normal_root;
  std::filesystem::path vault_root;
  std::filesystem::path state_root;
  std::filesystem::path cache_root;
};

enum class EntryType { Log, Memo, Todo };

enum class StorageErrorCode {
  InvalidPath,
  InvalidTimestamp,
  InvalidEntryType,
  IoError,
  DurabilityFailure
};

struct StorageError {
  StorageErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
  // A durability failure after replacement does not mean the old file remains.
  bool replacement_completed = false;
};

template <class T>
using Result = std::variant<T, StorageError>;

// Validation does not create directories. Existing symlink prefixes are resolved
// when checking that data roots and the state root do not overlap.
[[nodiscard]] Result<StoragePaths> validate_storage_paths(const StoragePaths& paths);

[[nodiscard]] Result<std::string> normal_entry_filename(
    EntryType type, const core::Id& id, core::Timestamp created);

// The parent directory must already exist. Replaces regular files or creates a
// new file; directories and symlinks are rejected. Only this call's temporary
// file is cleaned up on ordinary failure; crash recovery is outside this API.
[[nodiscard]] Result<std::monostate> write_file_atomic(
    const std::filesystem::path& path, std::string_view contents);

}  // namespace folio::storage
