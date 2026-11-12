#pragma once

#include "folio/core/directory.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"
#include "folio/core/todo.hpp"
#include "storage/common.hpp"
#include "storage/metadata_catalog.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace folio::core::storage {

// A filesystem object excluded from the scan, with its relative path and reason.
// Reserved atomic temporary files are omitted without producing this report.
struct ExcludedObject {
  std::filesystem::path path;
  std::string message;
};
struct NormalRepositoryScanResult {
  std::deque<LogMetadata> logs;
  std::deque<MemoMetadata> memos;
  std::deque<TodoMetadata> todos;
  std::deque<DirectoryMetadata> directories;
  std::vector<ExcludedObject> diagnostics;
};

// Mutation classification is independent of the underlying diagnostic. A failed
// operation may have published data without establishing crash durability.
enum class MutationFailureStage {
  BeforeAuthoritativeChange,
  AuthoritativeStateMayHaveChanged,
  CatalogSynchronizationFailed
};

enum class PersistenceObservation {
  NotAttempted,
  NotCompleted,
  Published,
  DurabilityConfirmed,
  Unknown
};
struct RepositoryMutationEvidence {
  PersistenceObservation write = PersistenceObservation::NotAttempted;
  PersistenceObservation destination = PersistenceObservation::NotAttempted;
  PersistenceObservation source_removal = PersistenceObservation::NotAttempted;
};
struct RepositoryMutationError {
  MutationFailureStage stage;
  RepositoryError cause;
  RepositoryMutationEvidence evidence;
};

// Borrowed payloads are valid only during synchronous invocation. Each operation
// invokes a nonempty callback once: saved metadata or a classified failure.
// Projection errors notify failure; callback errors are never notified recursively.
template <class Metadata>
using MutationNotification = std::variant<std::reference_wrapper<const Metadata>,
                                          std::reference_wrapper<const RepositoryMutationError>>;

template <class Metadata>
using MutationResultCallback = std::function<::folio::core::Result<std::monostate, StorageError>(
    const MutationNotification<Metadata>&)>;

// Checks UUID availability before creation. A managed caller supplies a Catalog
// check_creation plus the matching mutation callback, under single-writer execution.
// Without check_creation, creation retains filesystem-wide standalone uniqueness checks.
using CreationCheck =
    std::function<::folio::core::Result<std::monostate, StorageError>(const core::Id&)>;

// Canonical Normal Record filename; existing locators may use other filenames.
[[nodiscard]] ::folio::core::Result<std::string, StorageError>
normal_record_filename(RecordType type, const core::Id& id, core::Timestamp created);

// Synchronous plaintext persistence. The root is supplied by the caller.
// Managed operations must supply the matching Coordinator callback. Repository
// stores no callback; an empty callback skips projection and consistency handling.
// Callbacks must not retain borrowed payloads or re-enter this Repository.
// Entry locators returned here are relative to normal_root. The caller must
// exclude external filesystem mutation during an operation, as required by
// the single-file atomic writer. Stored UUIDs are authoritative for entries;
// existing entry filenames may differ from generated canonical names, while
// Logs/Memos require .md and Todos require .toml. Generated atomic temporary
// names lack those extensions, and .folio-dir.toml is Directory metadata.
class NormalRepository {
public:
  explicit NormalRepository(std::filesystem::path normal_root);

  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  create_log(const core::Log::Snapshot& snapshot,
             const MutationResultCallback<LogMetadata>& on_result = {},
             const CreationCheck& check_creation = {});
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  create_memo(const core::Memo::Snapshot& snapshot,
              const MutationResultCallback<MemoMetadata>& on_result = {},
              const CreationCheck& check_creation = {});
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  create_todo(const core::Todo::Snapshot& snapshot,
              const MutationResultCallback<TodoMetadata>& on_result = {},
              const CreationCheck& check_creation = {});

  // A supplied locator is read directly, without discovery or fallback.
  // It must still contain the requested stored UUID.
  [[nodiscard]] StorageResult<core::Log::Snapshot>
  load_log(const core::Id& id,
           std::optional<std::filesystem::path> relative_path = std::nullopt) const;
  [[nodiscard]] StorageResult<core::Memo::Snapshot>
  load_memo(const core::Id& id,
            std::optional<std::filesystem::path> relative_path = std::nullopt) const;
  [[nodiscard]] StorageResult<core::Todo::Snapshot>
  load_todo(const core::Id& id,
            std::optional<std::filesystem::path> relative_path = std::nullopt) const;

  /// Replaces the complete serialized Snapshot at the current authoritative path.
  /// Reads and validates existing identity/membership first; changing membership
  /// requires Move. Readable unknown fields reject rewriting before persistence.
  /// A supplied stale locator never triggers fallback discovery.
  /// Use the matching Coordinator callback in managed Backend execution.
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  update_log(const core::Log::Snapshot& snapshot,
             std::optional<std::filesystem::path> relative_path = std::nullopt,
             const MutationResultCallback<LogMetadata>& on_result = {});
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  update_memo(const core::Memo::Snapshot& snapshot,
              std::optional<std::filesystem::path> relative_path = std::nullopt,
              const MutationResultCallback<MemoMetadata>& on_result = {});
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  update_todo(const core::Todo::Snapshot& snapshot,
              std::optional<std::filesystem::path> relative_path = std::nullopt,
              const MutationResultCallback<TodoMetadata>& on_result = {});

  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  create_directory(DirectoryKind kind, const core::Directory::Snapshot& snapshot,
                   const MutationResultCallback<DirectoryMetadata>& on_result = {},
                   const CreationCheck& check_creation = {});
  // Directory kind and UUID determine the path; validate only that metadata.
  // Global conflicts are detected by explicit discovery/full reconstruction.
  [[nodiscard]] StorageResult<core::Directory::Snapshot> load_directory(DirectoryKind kind,
                                                                        const core::Id& id) const;
  /// Rejects readable metadata extensions before replacement to preserve unknown fields.
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  rename_directory(DirectoryKind kind, const core::Id& id, std::string name,
                   const MutationResultCallback<DirectoryMetadata>& on_result = {});

  /// Moves using destination creation followed by source removal, not a single
  /// atomic rename. Preserves editable fields and applies supplied membership and
  /// modified context; failure may leave durable partial effects or uncertainty.
  /// Unknown source properties reject the Mutation before destination creation.
  /// Supplied source locators use direct UUID validation; omission requests
  /// discovery. Managed callers supply the current Catalog locator and callback.
  /// Stale supplied locators never fall back. See docs/contracts/mutations.md.
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  move_memo(const core::Id& id, std::optional<core::Id> directory_id,
            core::TimestampContext modified,
            const MutationResultCallback<MemoMetadata>& on_result = {},
            std::optional<std::filesystem::path> relative_path = std::nullopt);
  [[nodiscard]] ::folio::core::Result<std::monostate, RepositoryMutationError>
  move_todo(const core::Id& id, std::optional<core::Id> directory_id,
            core::TimestampContext modified,
            const MutationResultCallback<TodoMetadata>& on_result = {},
            std::optional<std::filesystem::path> relative_path = std::nullopt);

  [[nodiscard]] StorageResult<std::filesystem::path> locate_record(RecordType type,
                                                                   const core::Id& id) const;
  [[nodiscard]] StorageResult<std::filesystem::path>
  locate_record(RecordType type, const std::filesystem::path& relative_path) const;

  // Collects authoritative metadata for a complete MetadataCatalog rebuild when
  // the Index is unavailable or an explicit Refresh is requested. Unlike UUID
  // discovery, this discovers Entries whose IDs are not yet known to the Catalog.
  // CatalogCoordinator converts the complete typed result for
  // MetadataCatalog::rebuild(); this method does not mutate the Catalog or
  // persist the Index. Do not call it for
  // ordinary CRUD, queries, or directory navigation.
  // Reads formal files and returns metadata with actual relative locators.
  // Absent type scans the entire Repository. A selected Directory includes its
  // metadata and immediate members; recursive controls Root member traversal.
  // Snapshots are projected per file; bodies are not retained in the result.
  // Missing roots/modules are empty. Unrelated objects produce diagnostics;
  // malformed managed content, duplicate identities, and unsafe paths fail.
  [[nodiscard]] StorageResult<NormalRepositoryScanResult>
  scan(std::optional<RecordType> type = {}, std::optional<core::Id> directory = {},
       bool recursive = true) const;
  /// Raw immediate entry count, with safe-path validation but no file classification.
  /// Missing module Roots count as empty; missing user Directories are errors.
  [[nodiscard]] StorageResult<std::size_t>
  count_entries(RecordType type, std::optional<core::Id> directory = {}) const;

private:
  std::filesystem::path root_;
};

} // namespace folio::core::storage
