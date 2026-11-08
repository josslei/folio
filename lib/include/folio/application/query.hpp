#pragma once

#include <folio/core/metadata.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace folio::application {

// Logical directory for navigation: a module Root or a persisted Directory ID.
// Roots need no fabricated identity and do not become persisted Entries.
using DirectoryRef = std::variant<core::RecordType, core::Id>;

/// Immediate logical contents, not recursive search results. Persisted Directories
/// cannot contain nested Directories, so their directories collection is empty.
struct BrowseResult {
  std::vector<core::RecordMetadata> records;
  std::vector<core::DirectoryMetadata> directories;
};

struct RootScope {};
struct AllScope {};
struct DirectoryScope {
  core::Id id;
};
using RecordScope = std::variant<RootScope, DirectoryScope, AllScope>;

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

struct RecordOrder {
  MetadataSort sort = MetadataSort::Created;
  bool descending = false;
};

struct RecordFilters {
  std::optional<core::Privacy> privacy;
  // ASCII case-insensitive substring matching; other UTF-8 bytes compare exactly.
  std::string title_contains;
  std::optional<core::Tag> tag;
  InclusiveRange<core::Timestamp> created;
  InclusiveRange<core::Timestamp> modified;
  InclusiveRange<std::chrono::year_month_day> log_date;
  std::optional<core::TodoStatus> todo_status;
  std::optional<core::TodoPriority> todo_priority;
  std::optional<bool> has_due_date;
  std::optional<DueKind> due_kind;
  std::optional<core::DueDate> due_from;
  std::optional<core::DueDate> due_through;
};

/// Metadata-only filtering; the default scope includes Root and Directory members.
/// This does not search Record bodies or trigger authoritative repository scans.
struct RecordQuery {
  std::optional<core::RecordType> type;
  RecordScope scope = AllScope{};
  RecordFilters filters;
  RecordOrder order;
};

} // namespace folio::application
