#pragma once

#include "folio/core/directory.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"
#include "folio/core/todo.hpp"
#include "storage/filesystem.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace folio::core::storage {

struct LogMetadata {
  Id id;
  std::filesystem::path relative_path;
  Privacy privacy;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  std::vector<Tag> tags;
  DateContext log_date;
};

struct MemoMetadata {
  Id id;
  std::filesystem::path relative_path;
  Privacy privacy;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  std::vector<Tag> tags;
  std::optional<Id> directory_id;
};

struct TodoMetadata {
  Id id;
  std::filesystem::path relative_path;
  Privacy privacy;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  TodoStatus status;
  TodoPriority priority;
  std::optional<DueDate> due_date;
  std::optional<Id> directory_id;
};

struct DirectoryMetadata {
  Id id;
  DirectoryKind kind;
  std::filesystem::path relative_path;
  std::string name;
};

using EntryMetadata = std::variant<LogMetadata, MemoMetadata, TodoMetadata>;
[[nodiscard]] EntryType entry_type(const EntryMetadata& metadata) noexcept;

template <class T>
struct InclusiveRange {
  std::optional<T> from;
  std::optional<T> through;
};

enum class DueKind { Date, Datetime };
enum class MetadataSort {
  Id, Type, Created, Modified, Title, LogDate, DueDate, Status, Priority, Privacy
};

struct CatalogQuery {
  std::optional<EntryType> type;
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
  std::vector<EntryMetadata> entries;
  std::vector<DirectoryMetadata> directories;
};

[[nodiscard]] EntryMetadata metadata_from(const Log::Snapshot& snapshot,
                                          std::filesystem::path relative_path);
[[nodiscard]] EntryMetadata metadata_from(const Memo::Snapshot& snapshot,
                                          std::filesystem::path relative_path);
[[nodiscard]] EntryMetadata metadata_from(const Todo::Snapshot& snapshot,
                                          std::filesystem::path relative_path);
[[nodiscard]] DirectoryMetadata metadata_from(const Directory::Snapshot& snapshot,
                                              std::filesystem::path relative_path);

// In-memory metadata only. Entry and Directory UUID registries are separate.
// Locators are lexically safe relative paths; physical layout belongs to the
// repository. Lookups return copies, so callers cannot mutate catalog state
// indirectly. UUID lookup and Directory membership listing use ordered IDs.
class MetadataCatalog {
 public:
  [[nodiscard]] Result<std::monostate> insert_entry(EntryMetadata metadata);
  [[nodiscard]] Result<std::monostate> update_entry(EntryMetadata metadata);
  [[nodiscard]] Result<std::monostate> remove_entry(const Id& id);
  [[nodiscard]] Result<std::monostate> insert_directory(DirectoryMetadata metadata);
  [[nodiscard]] Result<std::monostate> update_directory(DirectoryMetadata metadata);
  [[nodiscard]] Result<std::monostate> remove_directory(const Id& id);

  [[nodiscard]] std::optional<EntryMetadata> find_entry(const Id& id) const;
  [[nodiscard]] std::optional<DirectoryMetadata> find_directory(const Id& id) const;
  [[nodiscard]] RootListing list_root(EntryType module) const;
  [[nodiscard]] Result<std::vector<EntryMetadata>> list_directory(
      DirectoryKind kind, const Id& id) const;
  [[nodiscard]] std::vector<DirectoryMetadata> list_directories(DirectoryKind kind) const;
  [[nodiscard]] std::vector<EntryMetadata> query(const CatalogQuery& query = {}) const;
  [[nodiscard]] bool dirty() const noexcept;
  void mark_clean() noexcept;

 private:
  [[nodiscard]] bool entry_path_occupied(const std::filesystem::path& path,
                                         std::optional<Id> except = std::nullopt) const;
  [[nodiscard]] bool directory_referenced(const DirectoryMetadata& directory) const;
  [[nodiscard]] std::set<std::string>& members_for(EntryType type,
                                                  std::optional<Id> directory_id);

  std::map<std::string, LogMetadata> logs_;
  std::map<std::string, MemoMetadata> memos_;
  std::map<std::string, TodoMetadata> todos_;
  std::map<std::string, DirectoryMetadata> directories_;
  std::set<std::string> log_roots_;
  std::set<std::string> memo_roots_;
  std::set<std::string> todo_roots_;
  std::map<std::string, std::set<std::string>> directory_members_;
  bool dirty_ = false;
};

}  // namespace folio::core::storage
