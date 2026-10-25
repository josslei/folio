#include "folio/core/todo.hpp"

#include <stdexcept>
#include <utility>

namespace folio::core {

Todo::Todo(std::string description, TimestampContext created)
    : id_(Id::generate()),
      created_(std::move(created)),
      modified_(created_),
      description_(std::move(description)) {}

const Id& Todo::id() const noexcept { return id_; }
const std::string& Todo::description() const noexcept { return description_; }
const TimestampContext& Todo::created() const noexcept { return created_; }
const TimestampContext& Todo::modified() const noexcept { return modified_; }
TodoStatus Todo::status() const noexcept { return status_; }
TodoPriority Todo::priority() const noexcept { return priority_; }
const std::optional<DueDate>& Todo::due_date() const noexcept { return due_date_; }
const std::optional<Id>& Todo::directory_id() const noexcept { return directory_id_; }
Privacy Todo::privacy() const noexcept { return privacy_; }
const std::vector<Subtask>& Todo::subtasks() const noexcept { return subtasks_; }

void Todo::set_description(std::string description, TimestampContext modified) {
  description_ = std::move(description);
  modified_ = std::move(modified);
}

void Todo::set_status(TodoStatus status, TimestampContext modified) {
  status_ = status;
  modified_ = std::move(modified);
}

void Todo::set_priority(TodoPriority priority, TimestampContext modified) {
  priority_ = priority;
  modified_ = std::move(modified);
}

void Todo::set_due_date(std::optional<DueDate> due_date, TimestampContext modified) {
  if (due_date && std::holds_alternative<DateContext>(*due_date) &&
      !std::get<DateContext>(*due_date).date.ok()) {
    throw std::invalid_argument("Todo due date must be a valid calendar date");
  }
  due_date_ = std::move(due_date);
  modified_ = std::move(modified);
}

void Todo::set_directory_id(std::optional<Id> directory_id,
                            TimestampContext modified) {
  directory_id_ = std::move(directory_id);
  modified_ = std::move(modified);
}

void Todo::set_privacy(Privacy privacy, TimestampContext modified) {
  privacy_ = privacy;
  modified_ = std::move(modified);
}

void Todo::add_subtask(std::string content, TimestampContext modified) {
  subtasks_.push_back(Subtask{std::move(content)});
  modified_ = std::move(modified);
}

void Todo::set_subtask_content(std::size_t index, std::string content,
                               TimestampContext modified) {
  subtasks_.at(index).content = std::move(content);
  modified_ = std::move(modified);
}

void Todo::set_subtask_completed(std::size_t index, bool completed,
                                 TimestampContext modified) {
  subtasks_.at(index).completed = completed;
  modified_ = std::move(modified);
}

void Todo::remove_subtask(std::size_t index, TimestampContext modified) {
  if (index >= subtasks_.size()) {
    throw std::out_of_range("Subtask index is out of range");
  }
  subtasks_.erase(subtasks_.begin() + static_cast<std::ptrdiff_t>(index));
  modified_ = std::move(modified);
}

}  // namespace folio::core
