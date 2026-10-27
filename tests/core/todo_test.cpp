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
  const Todo todo("Plan trip", "", created);
  const Todo another("Plan trip", "", created);

  EXPECT_NE(todo.id(), another.id());
  EXPECT_EQ(todo.title(), "Plan trip");
  EXPECT_TRUE(todo.notes().empty());
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
  Todo todo("Task", "", created);
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
  Todo todo("Task", "", created);
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
  Todo todo("First", "", created);
  const Id original_id = todo.id();
  const Id directory_id = Id::generate();

  todo.set_title("Caf\u00e9 errand", modified);
  todo.set_notes("Plan:\n旅行\nCafé", modified);
  todo.set_priority(TodoPriority::High, modified);
  todo.set_directory_id(directory_id, modified);
  todo.set_status(TodoStatus::Completed, modified);

  EXPECT_EQ(todo.id(), original_id);
  EXPECT_EQ(todo.created().timestamp, created.timestamp);
  EXPECT_EQ(todo.created().timezone, created.timezone);
  EXPECT_EQ(todo.created().location, created.location);
  EXPECT_EQ(todo.modified().timestamp, modified.timestamp);
  EXPECT_EQ(todo.modified().timezone, modified.timezone);
  EXPECT_EQ(todo.modified().location, modified.location);
  EXPECT_EQ(todo.title(), "Caf\u00e9 errand");
  EXPECT_EQ(todo.notes(), "Plan:\n旅行\nCafé");
  EXPECT_EQ(todo.priority(), TodoPriority::High);
  EXPECT_EQ(todo.directory_id(), std::optional<Id>{directory_id});
  EXPECT_EQ(todo.privacy(), Privacy::Normal);
  EXPECT_EQ(todo.status(), TodoStatus::Completed);

  todo.set_directory_id(std::nullopt, modified);
  EXPECT_FALSE(todo.directory_id().has_value());
}

TEST(TodoTest, AcceptsExplicitPrivacyAtCreation) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                std::nullopt};
  const Todo todo("Task", "", created, Privacy::Hidden);

  EXPECT_EQ(todo.privacy(), Privacy::Hidden);
}

TEST(TodoTest, AllowsEmptyTitleAndNotesAndUpdatesEachModificationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const TimestampContext title_changed{Timestamp{std::chrono::seconds{200}},
                                       "Asia/Shanghai", "上海"};
  const TimestampContext notes_changed{Timestamp{std::chrono::seconds{300}},
                                       "Europe/Paris", "Paris"};
  Todo todo("", "", created);
  EXPECT_TRUE(todo.title().empty());
  EXPECT_TRUE(todo.notes().empty());
  const auto original_id = todo.id();

  todo.set_title("旅行", title_changed);
  EXPECT_EQ(todo.title(), "旅行");
  EXPECT_TRUE(todo.notes().empty());
  EXPECT_EQ(todo.modified().timestamp, title_changed.timestamp);
  EXPECT_EQ(todo.modified().timezone, title_changed.timezone);
  EXPECT_EQ(todo.modified().location, title_changed.location);

  todo.set_notes("First line\n第二行\nCafé", notes_changed);
  EXPECT_EQ(todo.title(), "旅行");
  EXPECT_EQ(todo.notes(), "First line\n第二行\nCafé");
  EXPECT_EQ(todo.modified().timestamp, notes_changed.timestamp);
  EXPECT_EQ(todo.modified().timezone, notes_changed.timezone);
  EXPECT_EQ(todo.modified().location, notes_changed.location);
  EXPECT_EQ(todo.created().timestamp, created.timestamp);
  EXPECT_EQ(todo.id(), original_id);

  todo.set_title("", notes_changed);
  todo.set_notes("", notes_changed);
  EXPECT_TRUE(todo.title().empty());
  EXPECT_TRUE(todo.notes().empty());
}

TEST(TodoTest, SubtasksStayOrderedAndParentCompletionDoesNotCompleteThem) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Todo todo("Task", "", created);
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
  Todo todo("Task", "", created);
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

TEST(TodoTest, SnapshotRestorePreservesAllFieldsAndExactContexts) {
  const TimestampContext created{Timestamp{std::chrono::milliseconds{123456789}},
                                 "Asia/Shanghai", std::string("Shanghai")};
  const TimestampContext modified{Timestamp{std::chrono::milliseconds{987654321}},
                                  "Europe/Paris", std::string("Paris")};
  Todo todo("Initial task", "First notes", created, Privacy::Hidden);
  todo.set_title("Caf\u00e9 task", modified);
  todo.set_notes("Second notes\n旅行", modified);
  todo.set_status(TodoStatus::Completed, modified);
  todo.set_priority(TodoPriority::High, modified);
  const DateContext due_date{std::chrono::year{2027} / 6 / 15,
                             "America/Los_Angeles", std::string("Los Angeles")};
  todo.set_due_date(DueDate{due_date}, modified);
  const Id directory = Id::generate();
  todo.set_directory_id(directory, modified);
  todo.add_subtask("First", modified);
  todo.add_subtask("Second", modified);
  todo.set_subtask_completed(1, true, modified);
  const auto snapshot = todo.snapshot();

  const auto restored = Todo::restore(snapshot);
  const auto restored_snapshot = restored.snapshot();

  EXPECT_EQ(restored_snapshot.id, snapshot.id);
  EXPECT_EQ(restored_snapshot.created.timestamp, snapshot.created.timestamp);
  EXPECT_EQ(restored_snapshot.created.timezone, snapshot.created.timezone);
  EXPECT_EQ(restored_snapshot.created.location, snapshot.created.location);
  EXPECT_EQ(restored_snapshot.modified.timestamp, snapshot.modified.timestamp);
  EXPECT_EQ(restored_snapshot.modified.timezone, snapshot.modified.timezone);
  EXPECT_EQ(restored_snapshot.modified.location, snapshot.modified.location);
  EXPECT_EQ(restored_snapshot.title, snapshot.title);
  EXPECT_EQ(restored_snapshot.notes, snapshot.notes);
  EXPECT_EQ(restored_snapshot.status, snapshot.status);
  EXPECT_EQ(restored_snapshot.priority, snapshot.priority);
  ASSERT_TRUE(restored_snapshot.due_date.has_value());
  ASSERT_TRUE(std::holds_alternative<DateContext>(*restored_snapshot.due_date));
  const auto& restored_due = std::get<DateContext>(*restored_snapshot.due_date);
  EXPECT_EQ(restored_due.date, due_date.date);
  EXPECT_EQ(restored_due.timezone, due_date.timezone);
  EXPECT_EQ(restored_due.location, due_date.location);
  EXPECT_EQ(restored_snapshot.directory_id, snapshot.directory_id);
  EXPECT_EQ(restored_snapshot.privacy, snapshot.privacy);
  ASSERT_EQ(restored_snapshot.subtasks.size(), snapshot.subtasks.size());
  for (std::size_t index = 0; index < snapshot.subtasks.size(); ++index) {
    EXPECT_EQ(restored_snapshot.subtasks[index].content,
              snapshot.subtasks[index].content);
    EXPECT_EQ(restored_snapshot.subtasks[index].completed,
              snapshot.subtasks[index].completed);
  }
  EXPECT_EQ(todo.snapshot().modified.timestamp, modified.timestamp);
  ASSERT_EQ(todo.snapshot().subtasks.size(), snapshot.subtasks.size());
  for (std::size_t index = 0; index < snapshot.subtasks.size(); ++index) {
    EXPECT_EQ(todo.snapshot().subtasks[index].content,
              snapshot.subtasks[index].content);
    EXPECT_EQ(todo.snapshot().subtasks[index].completed,
              snapshot.subtasks[index].completed);
  }
}

TEST(TodoTest, RestoreRejectsInvalidDateOnlyDueDate) {
  const TimestampContext context{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Todo todo("Task", "", context);
  auto snapshot = todo.snapshot();
  snapshot.due_date = DueDate{DateContext{
      std::chrono::year{2026} / 2 / 30, "UTC", std::nullopt}};

  EXPECT_THROW((void)Todo::restore(snapshot), std::invalid_argument);
}

TEST(TodoTest, SnapshotRestorePreservesDateTimeDueDate) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const TimestampContext due{Timestamp{std::chrono::milliseconds{987654321}},
                             "Europe/Paris", std::string("Paris")};
  Todo todo("Task", "", created);
  todo.set_due_date(DueDate{due},
                    {Timestamp{std::chrono::seconds{200}}, "UTC", std::nullopt});

  const auto restored = Todo::restore(todo.snapshot());

  ASSERT_TRUE(restored.due_date().has_value());
  ASSERT_TRUE(std::holds_alternative<TimestampContext>(*restored.due_date()));
  const auto& restored_due = std::get<TimestampContext>(*restored.due_date());
  EXPECT_EQ(restored_due.timestamp, due.timestamp);
  EXPECT_EQ(restored_due.timezone, due.timezone);
  EXPECT_EQ(restored_due.location, due.location);
}

TEST(TodoTest, RestoreRejectsInvalidStatusAndPriority) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const Todo todo("Task", "", created);
  auto snapshot = todo.snapshot();
  snapshot.status = static_cast<TodoStatus>(99);
  EXPECT_THROW((void)Todo::restore(snapshot), std::invalid_argument);

  snapshot = todo.snapshot();
  snapshot.priority = static_cast<TodoPriority>(99);
  EXPECT_THROW((void)Todo::restore(snapshot), std::invalid_argument);

  snapshot = todo.snapshot();
  snapshot.privacy = static_cast<Privacy>(99);
  EXPECT_THROW((void)Todo::restore(snapshot), std::invalid_argument);
}

}  // namespace
