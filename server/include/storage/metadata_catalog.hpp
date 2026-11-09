#pragma once

#include "folio/core/directory.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"
#include "folio/core/todo.hpp"
#include "storage/common.hpp"
#include "storage/located.hpp"
#include <folio/core/metadata.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace folio::core::storage {

// Private typed conveniences; shared fields are defined only in Core.
using LogMetadata = Located<core::LogMetadata>;
using MemoMetadata = Located<core::MemoMetadata>;
using TodoMetadata = Located<core::TodoMetadata>;
using DirectoryMetadata = Located<core::DirectoryMetadata>;

// Records are Log/Memo/Todo Entries. Directory remains a first-class Entry
// with different fields; these types do not change existing query semantics.
using RecordMetadata = std::variant<LogMetadata, MemoMetadata, TodoMetadata>;
[[nodiscard]] RecordType record_type(const RecordMetadata& metadata) noexcept;

template <class T> struct InclusiveRange {
  std::optional<T> from;
  std::optional<T> through;
};

enum class DueKind { Date, Datetime };
enum class MetadataSort {
  Id,
  Type,
  Created,
  Modified,
  Title,
  LogDate,
  DueDate,
  Status,
  Priority,
  Privacy
};

// Filters only Record metadata; Directory listing remains a separate operation.
struct RecordQuery {
  std::optional<RecordType> type;
  // nullopt: any Directory; optional<nullopt>: module root; ID: that Directory.
  std::optional<std::optional<Id>> directory;
  std::optional<Privacy> privacy;
  // ASCII case insensitive substring in title. Other UTF-8 bytes
  // compare exactly. Tags use the same matching rule.
  std::string text;
  std::optional<Tag> tag;
  InclusiveRange<Timestamp> created;
  InclusiveRange<Timestamp> modified;
  InclusiveRange<std::chrono::year_month_day> log_date;
  std::optional<TodoStatus> todo_status;
  std::optional<TodoPriority> todo_priority;
  std::optional<bool> has_due_date;
  std::optional<DueKind> due_kind;
  // A bound applies only to due dates with the same variant alternative.
  std::optional<DueDate> due_from;
  std::optional<DueDate> due_through;
  // Missing or inapplicable sort fields come last in either direction. UUIDs
  // break equal keys. Due Date sorts before Datetime without timezone conversion.
  MetadataSort sort = MetadataSort::Created;
  bool descending = false;
};

struct RootListing {
  std::vector<RecordMetadata> records;
  std::vector<DirectoryMetadata> directories;
};

[[nodiscard]] RecordMetadata metadata_from(const Log::Snapshot& snapshot,
                                           std::filesystem::path relative_path);
[[nodiscard]] RecordMetadata metadata_from(const Memo::Snapshot& snapshot,
                                           std::filesystem::path relative_path);
[[nodiscard]] RecordMetadata metadata_from(const Todo::Snapshot& snapshot,
                                           std::filesystem::path relative_path);
[[nodiscard]] DirectoryMetadata metadata_from(const Directory::Snapshot& snapshot,
                                              std::filesystem::path relative_path);

// Reuse Catalog field validation before authoritative publication. These checks
// perform no I/O and do not insert/update metadata or validate live membership.
[[nodiscard]] core::Result<std::monostate, StorageError>
validate_metadata(const RecordMetadata& metadata);
[[nodiscard]] core::Result<std::monostate, StorageError>
validate_metadata(const DirectoryMetadata& metadata);

// Compare existing projected fields/locators without reading authoritative data.
[[nodiscard]] bool metadata_matches(const RecordMetadata& left, const RecordMetadata& right);

class MetadataCatalogView;

/// In-memory metadata only. All four Entry types share one UUID identity namespace.
/// Capture, mutation and Index bookkeeping remain executor-only. Retained views
/// hold immutable state independently of subsequent publication.
// Locators are lexically safe relative paths; physical layout belongs to the
// repository. Lookups return copies, so callers cannot mutate catalog state
// indirectly. UUID lookup and Directory membership listing use ordered IDs.
class MetadataCatalog {
public:
  MetadataCatalog();

  /// Retains immutable container roots; later writes do not change this view.
  [[nodiscard]] MetadataCatalogView read_view() const;

  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError>
  insert_record(RecordMetadata metadata);
  // Identical updates are successful no-ops and preserve unpersisted state.
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError>
  update_record(RecordMetadata metadata);
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError> remove_record(const Id& id);
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError>
  insert_directory(DirectoryMetadata metadata);
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError>
  update_directory(DirectoryMetadata metadata);
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError> remove_directory(const Id& id);
  // Validates a complete candidate in isolation, then adopts it atomically.
  // A successful replacement is unpersisted even if all vectors are empty.
  [[nodiscard]] ::folio::core::Result<std::monostate, StorageError>
  rebuild(std::vector<LogMetadata> logs, std::vector<MemoMetadata> memos,
          std::vector<TodoMetadata> todos, std::vector<DirectoryMetadata> directories);

  // Changes since the last acknowledged Index snapshot; unrelated to readiness
  // or Git synchronization. Acknowledge only after load or successful persistence.
  [[nodiscard]] bool unpersisted() const noexcept;
  void mark_persisted() noexcept;

private:
  friend class MetadataCatalogView;
  struct State;
  /// Build in isolation; publish only a successful, non-identical candidate.
  template <class Change> core::Result<std::monostate, StorageError> change(Change&& apply);

  core::Result<std::monostate, StorageError> remove_directory_candidate(State& next, const Id& id);
  core::Result<std::monostate, StorageError> update_directory_candidate(State& next,
                                                                        DirectoryMetadata metadata);
  core::Result<std::monostate, StorageError> insert_directory_candidate(State& next,
                                                                        DirectoryMetadata metadata);
  core::Result<std::monostate, StorageError> remove_record_candidate(State& next, const Id& id);
  core::Result<std::monostate, StorageError> update_record_candidate(State& next,
                                                                     RecordMetadata metadata);
  core::Result<std::monostate, StorageError> insert_record_candidate(State& next,
                                                                     RecordMetadata metadata);
  [[nodiscard]] bool record_path_occupied(const std::filesystem::path& path,
                                          std::optional<Id> except = std::nullopt) const;
  [[nodiscard]] bool directory_referenced(const DirectoryMetadata& directory) const;
  void change_members(State& next, RecordType type, std::optional<Id> directory_id,
                      const std::string& key, bool insert);

  // A candidate copies only persistent roots. Metadata boxes share large values.
  std::shared_ptr<const State> state_;
  const State& state() const { return *state_; }
  bool unpersisted_ = false;
};

/// Backend-private read capability. No mutators or Index bookkeeping are exposed.
// Views own their roots, so they can outlive the originating Catalog. Capture and
// publication remain executor-only until runtime synchronization is introduced.
class MetadataCatalogView {
public:
  // UUID membership across all four Entry types, without copying metadata.
  [[nodiscard]] bool contains_entry(const Id& id) const;
  [[nodiscard]] std::optional<RecordMetadata> find_record(const Id& id) const;
  [[nodiscard]] std::optional<DirectoryMetadata> find_directory(const Id& id) const;
  [[nodiscard]] RootListing list_root(RecordType module) const;
  [[nodiscard]] ::folio::core::Result<std::vector<RecordMetadata>, StorageError>
  list_directory(DirectoryKind kind, const Id& id) const;
  [[nodiscard]] std::vector<DirectoryMetadata> list_directories(DirectoryKind kind) const;
  [[nodiscard]] std::vector<RecordMetadata> query(const RecordQuery& query = {}) const;

private:
  friend class MetadataCatalog;
  explicit MetadataCatalogView(std::shared_ptr<const MetadataCatalog::State> state);
  std::shared_ptr<const MetadataCatalog::State> state_;
};

} // namespace folio::core::storage
