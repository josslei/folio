#pragma once

#include "folio/core/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace folio::core {

enum class TodoStatus { Pending, Completed };
enum class TodoPriority { None, Low, Medium, High };

using DueDate = std::variant<DateContext, TimestampContext>;

struct Subtask {
  std::string content;  // UTF-8.
  bool completed = false;
};

class Todo {
 public:
  struct Snapshot {
    Id id;
    TimestampContext created;
    TimestampContext modified;
    std::string description;
    TodoStatus status;
    TodoPriority priority;
    std::optional<DueDate> due_date;
    std::optional<Id> directory_id;
    Privacy privacy;
    std::vector<Subtask> subtasks;
  };

  Todo(std::string description, TimestampContext created,
       Privacy privacy = Privacy::Normal);

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] static Todo restore(const Snapshot& snapshot);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] const std::string& description() const noexcept;
  [[nodiscard]] const TimestampContext& created() const noexcept;
  [[nodiscard]] const TimestampContext& modified() const noexcept;
  [[nodiscard]] TodoStatus status() const noexcept;
  [[nodiscard]] TodoPriority priority() const noexcept;
  [[nodiscard]] const std::optional<DueDate>& due_date() const noexcept;
  [[nodiscard]] const std::optional<Id>& directory_id() const noexcept;
  [[nodiscard]] Privacy privacy() const noexcept;
  [[nodiscard]] const std::vector<Subtask>& subtasks() const noexcept;

  void set_description(std::string description, TimestampContext modified);
  void set_status(TodoStatus status, TimestampContext modified);
  void set_priority(TodoPriority priority, TimestampContext modified);
  void set_due_date(std::optional<DueDate> due_date, TimestampContext modified);
  void set_directory_id(std::optional<Id> directory_id, TimestampContext modified);

  void add_subtask(std::string content, TimestampContext modified);
  void set_subtask_content(std::size_t index, std::string content,
                           TimestampContext modified);
  void set_subtask_completed(std::size_t index, bool completed,
                             TimestampContext modified);
  void remove_subtask(std::size_t index, TimestampContext modified);

 private:
  explicit Todo(const Snapshot& snapshot);

  const Id id_;
  const TimestampContext created_;
  TimestampContext modified_;
  std::string description_;
  TodoStatus status_ = TodoStatus::Pending;
  TodoPriority priority_ = TodoPriority::None;
  std::optional<DueDate> due_date_;
  std::optional<Id> directory_id_;
  const Privacy privacy_;
  std::vector<Subtask> subtasks_;
};

}  // namespace folio::core
