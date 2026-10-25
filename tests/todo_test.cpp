#include "folio/core/todo.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using folio::core::DateContext;
using folio::core::DueDate;
using folio::core::Id;
using folio::core::Privacy;
using folio::core::Subtask;
using folio::core::Timestamp;
using folio::core::TimestampContext;
using folio::core::Todo;
using folio::core::TodoPriority;
using folio::core::TodoStatus;

TEST(SubtaskTest, StartsIncompleteWithOnlyContent) {
  const Subtask subtask{"Draft outline"};

  EXPECT_EQ(subtask.content, "Draft outline");
  EXPECT_FALSE(subtask.completed);
}

TEST(TodoTest, StartsWithDefaultsAndDistinctIdentity) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}},
                                 "Asia/Shanghai", std::string("Shanghai")};
  const Todo todo("Plan trip", created);
  const Todo another("Plan trip", created);

  EXPECT_NE(todo.id(), another.id());
  EXPECT_EQ(todo.description(), "Plan trip");
  EXPECT_EQ(todo.created().timestamp, created.timestamp);
  EXPECT_EQ(todo.created().timezone, created.timezone);
  EXPECT_EQ(todo.created().location, created.location);
  EXPECT_EQ(todo.modified().timestamp, created.timestamp);
  EXPECT_EQ(todo.modified().timezone, created.timezone);
  EXPECT_EQ(todo.modified().location, created.location);
  EXPECT_EQ(todo.status(), TodoStatus::Pending);
  EXPECT_EQ(todo.priority(), TodoPriority::None);
  EXPECT_FALSE(todo.due_date().has_value());
  EXPECT_FALSE(todo.directory_id().has_value());
  EXPECT_EQ(todo.privacy(), Privacy::Normal);
  EXPECT_TRUE(todo.subtasks().empty());
}

TEST(TodoTest, DueDateRetainsDateOnlyOrExactTimeAndCanBeCleared) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const TimestampContext modified{Timestamp{std::chrono::seconds{200}},
                                  "Europe/Paris", std::string("Paris")};
  Todo todo("Task", created);
  const DateContext calendar_due{std::chrono::year{2026} / 10 / 25,
                                 "Asia/Shanghai", std::string("Shanghai")};

  todo.set_due_date(DueDate{calendar_due}, modified);
  ASSERT_TRUE(todo.due_date().has_value());
  ASSERT_TRUE(std::holds_alternative<DateContext>(*todo.due_date()));
  EXPECT_EQ(std::get<DateContext>(*todo.due_date()).date, calendar_due.date);
  EXPECT_EQ(std::get<DateContext>(*todo.due_date()).timezone, calendar_due.timezone);
  EXPECT_EQ(std::get<DateContext>(*todo.due_date()).location, calendar_due.location);

  const TimestampContext timed_due{Timestamp{std::chrono::seconds{300}},
                                   "Europe/Paris", std::string("Paris")};
  todo.set_due_date(DueDate{timed_due}, modified);
  ASSERT_TRUE(todo.due_date().has_value());
  ASSERT_TRUE(std::holds_alternative<TimestampContext>(*todo.due_date()));
  EXPECT_EQ(std::get<TimestampContext>(*todo.due_date()).timestamp,
            timed_due.timestamp);
  EXPECT_EQ(std::get<TimestampContext>(*todo.due_date()).timezone,
            timed_due.timezone);
  EXPECT_EQ(std::get<TimestampContext>(*todo.due_date()).location,
            timed_due.location);

  todo.set_due_date(std::nullopt, modified);
  EXPECT_FALSE(todo.due_date().has_value());
  EXPECT_EQ(todo.modified().timestamp, modified.timestamp);
}

TEST(TodoTest, RejectsInvalidCalendarDueDateWithoutChangingState) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Todo todo("Task", created);
  const DateContext invalid_due{std::chrono::year{2026} / 2 / 30, "UTC",
                                std::nullopt};

  EXPECT_THROW(todo.set_due_date(DueDate{invalid_due},
                                 {Timestamp{std::chrono::seconds{200}}, "UTC",
                                  std::nullopt}),
               std::invalid_argument);
  EXPECT_FALSE(todo.due_date().has_value());
  EXPECT_EQ(todo.modified().timestamp, created.timestamp);
}

TEST(TodoTest, FieldChangesPreserveIdentityAndCreationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const TimestampContext modified{Timestamp{std::chrono::seconds{200}},
                                  "Europe/Paris", std::string("Paris")};
  Todo todo("First", created);
  const Id original_id = todo.id();
  const Id directory_id = Id::generate();

  todo.set_description("Caf\u00e9 errand", modified);
  todo.set_priority(TodoPriority::High, modified);
  todo.set_directory_id(directory_id, modified);
  todo.set_privacy(Privacy::Private, modified);
  todo.set_status(TodoStatus::Completed, modified);

  EXPECT_EQ(todo.id(), original_id);
  EXPECT_EQ(todo.created().timestamp, created.timestamp);
  EXPECT_EQ(todo.created().timezone, created.timezone);
  EXPECT_EQ(todo.created().location, created.location);
  EXPECT_EQ(todo.modified().timestamp, modified.timestamp);
  EXPECT_EQ(todo.modified().timezone, modified.timezone);
  EXPECT_EQ(todo.modified().location, modified.location);
  EXPECT_EQ(todo.description(), "Caf\u00e9 errand");
  EXPECT_EQ(todo.priority(), TodoPriority::High);
  EXPECT_EQ(todo.directory_id(), std::optional<Id>{directory_id});
  EXPECT_EQ(todo.privacy(), Privacy::Private);
  EXPECT_EQ(todo.status(), TodoStatus::Completed);

  todo.set_directory_id(std::nullopt, modified);
  EXPECT_FALSE(todo.directory_id().has_value());
}

TEST(TodoTest, SubtasksStayOrderedAndParentCompletionDoesNotCompleteThem) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Todo todo("Task", created);
  const TimestampContext first_change{Timestamp{std::chrono::seconds{200}}, "UTC",
                                      std::nullopt};
  const TimestampContext second_change{Timestamp{std::chrono::seconds{300}}, "UTC",
                                       std::nullopt};

  todo.add_subtask("First", first_change);
  EXPECT_EQ(todo.modified().timestamp, first_change.timestamp);
  todo.add_subtask("Second", second_change);
  ASSERT_EQ(todo.subtasks().size(), 2u);
  EXPECT_EQ(todo.subtasks()[0].content, "First");
  EXPECT_EQ(todo.subtasks()[1].content, "Second");
  EXPECT_FALSE(todo.subtasks()[0].completed);
  EXPECT_FALSE(todo.subtasks()[1].completed);
  EXPECT_EQ(todo.modified().timestamp, second_change.timestamp);

  todo.set_status(TodoStatus::Completed, second_change);
  EXPECT_FALSE(todo.subtasks()[0].completed);
  EXPECT_FALSE(todo.subtasks()[1].completed);
}

TEST(TodoTest, SubtaskEditsAndRemovalUpdateParentModificationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Todo todo("Task", created);
  todo.add_subtask("First", created);
  todo.add_subtask("Second", created);
  const TimestampContext edited{Timestamp{std::chrono::seconds{200}},
                                "Asia/Shanghai", std::string("Shanghai")};

  todo.set_subtask_content(0, "Revised", edited);
  EXPECT_EQ(todo.subtasks()[0].content, "Revised");
  EXPECT_EQ(todo.modified().timestamp, edited.timestamp);
  todo.set_subtask_completed(0, true, edited);
  EXPECT_TRUE(todo.subtasks()[0].completed);
  EXPECT_FALSE(todo.subtasks()[1].completed);
  EXPECT_EQ(todo.modified().timezone, edited.timezone);
  EXPECT_EQ(todo.modified().location, edited.location);

  todo.remove_subtask(0, edited);
  ASSERT_EQ(todo.subtasks().size(), 1u);
  EXPECT_EQ(todo.subtasks()[0].content, "Second");
  EXPECT_EQ(todo.modified().timestamp, edited.timestamp);

  const TimestampContext later{Timestamp{std::chrono::seconds{300}}, "UTC",
                               std::nullopt};
  EXPECT_THROW(todo.set_subtask_completed(1, true, later), std::out_of_range);
  EXPECT_THROW(todo.remove_subtask(1, later), std::out_of_range);
  EXPECT_EQ(todo.modified().timestamp, edited.timestamp);
  EXPECT_EQ(todo.subtasks().size(), 1u);
}

}  // namespace
