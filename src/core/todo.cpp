#include "folio/core/todo.hpp"
#include "internal/validation.hpp"

#include <stdexcept>
#include <utility>

namespace folio::core {
namespace {

bool is_valid_status(TodoStatus status) {
  return status == TodoStatus::Pending || status == TodoStatus::Completed;
}

bool is_valid_priority(TodoPriority priority) {
  switch (priority) {
    case TodoPriority::None:
    case TodoPriority::Low:
    case TodoPriority::Medium:
    case TodoPriority::High:
      return true;
  }
  return false;
}

void validate_snapshot(const Todo::Snapshot& snapshot) {
  if (!detail::is_valid_privacy(snapshot.privacy)) {
    throw std::invalid_argument("Todo snapshot privacy is invalid");
  }
  if (!is_valid_status(snapshot.status)) {
    throw std::invalid_argument("Todo snapshot status is invalid");
  }
  if (!is_valid_priority(snapshot.priority)) {
    throw std::invalid_argument("Todo snapshot priority is invalid");
  }
  if (snapshot.due_date && std::holds_alternative<DateContext>(*snapshot.due_date) &&
      !std::get<DateContext>(*snapshot.due_date).date.ok()) {
    throw std::invalid_argument("Todo snapshot due date must be a valid calendar date");
  }
}

}  // namespace

Todo::Todo(std::string description, TimestampContext created, Privacy privacy)
    : id_(Id::generate()),
      created_(std::move(created)),
      modified_(created_),
      description_(std::move(description)),
      privacy_(privacy) {}

Todo::Todo(const Snapshot& snapshot)
    : id_(snapshot.id),
      created_(snapshot.created),
      modified_(snapshot.modified),
      description_(snapshot.description),
      status_(snapshot.status),
      priority_(snapshot.priority),
      due_date_(snapshot.due_date),
      directory_id_(snapshot.directory_id),
      privacy_(snapshot.privacy),
      subtasks_(snapshot.subtasks) {
  validate_snapshot(snapshot);
}

Todo::Snapshot Todo::snapshot() const {
  return Snapshot{id_,       created_,    modified_, description_, status_,
                  priority_, due_date_,   directory_id_, privacy_, subtasks_};
}

Todo Todo::restore(const Snapshot& snapshot) { return Todo(snapshot); }

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
