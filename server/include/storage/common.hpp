#pragma once

#include <folio/core/result.hpp>
#include <folio/io/filesystem.hpp>
#include <folio/config/configuration.hpp>

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

// Effective application-owned directories, resolved during application bootstrap
// or explicitly injected by tests. No directories are
// created by constructing or validating these values. Runtime may be unavailable
// (including platforms whose IPC transport needs no filesystem directory).
using config::ApplicationPaths;
using config::StoragePaths;

using core::RecordType;
using core::DirectoryKind;

enum class StorageErrorCode {
  InvalidTimestamp,
  InvalidRecordType,
  InvalidLayout,
  CatalogUnavailable,
  OperationFailure,
  CodecError,
  IdentityMismatch,
  DuplicateId,
  StaleLocator,
  InvalidMetadata,
  MetadataNotFound,
  ReferencedDirectory,
  // Authoritative persistence succeeded, but metadata projection/callback failed.
  PostPersistFailure
};

struct StorageError {
  StorageErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
  // True if a filesystem change was published (including removal), even when a later sync or
  // cleanup failed. This does not by itself confirm crash durability.
  bool replacement_completed = false;
};

// Repository operations preserve filesystem and Storage diagnostics independently.
template <class T>
using StorageResult = std::variant<T, StorageError, io::FileError>;
using RepositoryError = std::variant<StorageError, io::FileError>;

// Validation does not create directories. Existing symlink prefixes are resolved
// when checking authoritative roots against each other and application paths.
// Application categories may share or nest directories. Data may be a strict
// ancestor of authoritative roots; other support directories must stay disjoint.
[[nodiscard]] StorageResult<StoragePaths> validate_storage_paths(const StoragePaths& paths);

}  // namespace folio::core::storage
