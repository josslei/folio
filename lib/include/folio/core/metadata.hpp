#pragma once

#include <folio/core/directory.hpp>
#include <folio/core/todo.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace folio::core {

// Navigation values contain no persistence locator or complete Record body.
struct LogMetadata {
  Id id;
  Privacy privacy;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  std::vector<Tag> tags;
  DateContext log_date;
};

struct MemoMetadata {
  Id id;
  Privacy privacy;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  std::vector<Tag> tags;
  std::optional<Id> directory_id;
};

struct TodoMetadata {
  Id id;
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
  std::string name;
};

// Complete, authorized metadata only. TODO(Vault): design locked placeholders
// and Directory visibility; never represent inaccessible fields with defaults.
using RecordMetadata = std::variant<LogMetadata, MemoMetadata, TodoMetadata>;

} // namespace folio::core
