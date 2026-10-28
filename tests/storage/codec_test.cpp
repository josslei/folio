#include "storage/codec.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace folio::core;
namespace codec = folio::core::storage::codec;

template <typename T>
const T& value_of(const ::folio::core::internal::Result<T, codec::CodecError>& result) {
  return std::get<T>(result);
}

template <typename T>
bool is_error(const ::folio::core::internal::Result<T, codec::CodecError>& result) {
  return std::holds_alternative<codec::CodecError>(result);
}

Id sample_id() {
  return *Id::parse("47183823-2574-4bfd-b411-99ed177d3e43");
}

TimestampContext sample_timestamp_context(std::optional<std::string> location) {
  return {Timestamp{std::chrono::milliseconds{123456789}}, "Asia/Shanghai",
          std::move(location)};
}

void expect_timestamp_context_eq(const TimestampContext& actual,
                                 const TimestampContext& expected) {
  EXPECT_EQ(actual.timestamp, expected.timestamp);
  EXPECT_EQ(actual.timezone, expected.timezone);
  EXPECT_EQ(actual.location, expected.location);
}

void expect_date_context_eq(const DateContext& actual,
                            const DateContext& expected) {
  EXPECT_EQ(actual.date, expected.date);
  EXPECT_EQ(actual.timezone, expected.timezone);
  EXPECT_EQ(actual.location, expected.location);
}

void expect_due_date_eq(const std::optional<DueDate>& actual,
                        const std::optional<DueDate>& expected) {
  ASSERT_EQ(actual.has_value(), expected.has_value());
  if (!actual) {
    return;
  }
  ASSERT_EQ(actual->index(), expected->index());
  if (std::holds_alternative<DateContext>(*expected)) {
    expect_date_context_eq(std::get<DateContext>(*actual),
                           std::get<DateContext>(*expected));
  } else {
    expect_timestamp_context_eq(std::get<TimestampContext>(*actual),
                                std::get<TimestampContext>(*expected));
  }
}

void expect_subtasks_eq(const std::vector<Subtask>& actual,
                        const std::vector<Subtask>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    EXPECT_EQ(actual[index].content, expected[index].content);
    EXPECT_EQ(actual[index].completed, expected[index].completed);
  }
}

TEST(CoreCodecScalarTest, RoundTripsIdTagAndPrivacy) {
  const auto id = sample_id();
  const auto parsed_id = codec::id_from_toml(codec::to_toml(id));
  ASSERT_FALSE(is_error(parsed_id));
  EXPECT_EQ(value_of(parsed_id), id);

  const Tag tag{"\xE7\xAC\x94\xE8\xAE\xB0"};
  const auto parsed_tag = codec::tag_from_toml(codec::to_toml(tag));
  ASSERT_FALSE(is_error(parsed_tag));
  EXPECT_EQ(value_of(parsed_tag), tag);

  for (const auto privacy : {Privacy::Normal, Privacy::Protected,
                             Privacy::Hidden}) {
    const auto parsed = codec::privacy_from_toml(codec::to_toml(privacy));
    ASSERT_FALSE(is_error(parsed));
    EXPECT_EQ(value_of(parsed), privacy);
  }
}

TEST(CoreCodecContextTest, RoundTripsContextsWithAndWithoutLocation) {
  const TimestampContext with_location =
      sample_timestamp_context(std::string("Shanghai"));
  const auto parsed_timestamp = codec::timestamp_context_from_toml(
      codec::to_toml(with_location));
  ASSERT_FALSE(is_error(parsed_timestamp));
  expect_timestamp_context_eq(value_of(parsed_timestamp), with_location);
  EXPECT_TRUE(codec::to_toml(with_location).contains("location"));

  const TimestampContext without_location = sample_timestamp_context(std::nullopt);
  const auto parsed_without_location = codec::timestamp_context_from_toml(
      codec::to_toml(without_location));
  ASSERT_FALSE(is_error(parsed_without_location));
  expect_timestamp_context_eq(value_of(parsed_without_location),
                               without_location);
  EXPECT_FALSE(codec::to_toml(without_location).contains("location"));

  const DateContext date{{std::chrono::year{2025} / 3 / 4}, "UTC", std::nullopt};
  const auto parsed_date = codec::date_context_from_toml(codec::to_toml(date));
  ASSERT_FALSE(is_error(parsed_date));
  expect_date_context_eq(value_of(parsed_date), date);
  EXPECT_FALSE(codec::to_toml(date).contains("location"));
}

TEST(CoreCodecContextTest, PreservesSubsecondTimestampPrecision) {
  const TimestampContext original{Timestamp{std::chrono::duration_cast<Timestamp::duration>(
                                      std::chrono::microseconds{987654})},
                                 "UTC", std::nullopt};
  const auto decoded =
      codec::timestamp_context_from_toml(codec::to_toml(original));

  ASSERT_FALSE(is_error(decoded));
  expect_timestamp_context_eq(value_of(decoded), original);
}

TEST(CoreCodecContextTest, RoundTripsBothDueDateAlternatives) {
  const DueDate date_only = DateContext{{std::chrono::year{2026} / 10 / 25},
                                        "Asia/Tokyo", std::string("Tokyo")};
  const auto parsed_date = codec::due_date_from_toml(codec::to_toml(date_only));
  ASSERT_FALSE(is_error(parsed_date));
  ASSERT_TRUE(std::holds_alternative<DateContext>(value_of(parsed_date)));
  expect_date_context_eq(std::get<DateContext>(value_of(parsed_date)),
                         std::get<DateContext>(date_only));
  EXPECT_EQ(codec::to_toml(date_only).get("kind")->value<std::string>(),
            std::optional<std::string>{"Date"});

  const DueDate date_time = sample_timestamp_context(std::nullopt);
  const auto parsed_date_time =
      codec::due_date_from_toml(codec::to_toml(date_time));
  ASSERT_FALSE(is_error(parsed_date_time));
  ASSERT_TRUE(std::holds_alternative<TimestampContext>(value_of(parsed_date_time)));
  expect_timestamp_context_eq(std::get<TimestampContext>(value_of(parsed_date_time)),
                              std::get<TimestampContext>(date_time));
  EXPECT_EQ(codec::to_toml(date_time).get("kind")->value<std::string>(),
            std::optional<std::string>{"Datetime"});
}

TEST(CoreCodecSnapshotTest, RoundTripsDefaultTodoAndOmittedOptionals) {
  Todo original("Default task", "", sample_timestamp_context(std::nullopt));
  const auto snapshot = original.snapshot();
  const auto table = codec::to_toml(snapshot);

  EXPECT_FALSE(table.contains("due"));
  EXPECT_FALSE(table.contains("directory_id"));
  EXPECT_FALSE(table.contains("subtasks"));
  EXPECT_FALSE(table.contains("description"));
  EXPECT_EQ(table.get("title")->value<std::string>(),
            std::optional<std::string>{"Default task"});
  EXPECT_EQ(table.get("notes")->value<std::string>(),
            std::optional<std::string>{""});
  EXPECT_EQ(table.get("status")->value<std::string>(),
            std::optional<std::string>{"Pending"});
  EXPECT_EQ(table.get("priority")->value<std::string>(),
            std::optional<std::string>{"None"});
  EXPECT_EQ(table.get("privacy")->value<std::string>(),
            std::optional<std::string>{"Normal"});

  const auto decoded = codec::todo_snapshot_from_toml(table);
  ASSERT_FALSE(is_error(decoded));
  const Todo restored = Todo::restore(value_of(decoded));
  const auto actual = restored.snapshot();
  EXPECT_EQ(actual.id, snapshot.id);
  expect_timestamp_context_eq(actual.created, snapshot.created);
  expect_timestamp_context_eq(actual.modified, snapshot.modified);
  EXPECT_EQ(actual.title, snapshot.title);
  EXPECT_EQ(actual.notes, snapshot.notes);
  EXPECT_EQ(actual.status, snapshot.status);
  EXPECT_EQ(actual.priority, snapshot.priority);
  expect_due_date_eq(actual.due_date, snapshot.due_date);
  EXPECT_EQ(actual.directory_id, snapshot.directory_id);
  EXPECT_EQ(actual.privacy, snapshot.privacy);
  expect_subtasks_eq(actual.subtasks, snapshot.subtasks);
}

TEST(CoreCodecSnapshotTest, PreservesEmptyTodoTitleAndNotesWithoutLegacyDescription) {
  const Todo original("", "", sample_timestamp_context(std::nullopt));
  const auto table = codec::to_toml(original.snapshot());
  EXPECT_EQ(table.get("title")->value<std::string>(),
            std::optional<std::string>{""});
  EXPECT_EQ(table.get("notes")->value<std::string>(),
            std::optional<std::string>{""});
  EXPECT_FALSE(table.contains("description"));
  const auto decoded = codec::todo_snapshot_from_toml(table);
  ASSERT_FALSE(is_error(decoded));
  EXPECT_TRUE(value_of(decoded).title.empty());
  EXPECT_TRUE(value_of(decoded).notes.empty());
  const auto restored = Todo::restore(value_of(decoded));
  EXPECT_TRUE(restored.title().empty());
  EXPECT_TRUE(restored.notes().empty());

  auto legacy = table;
  legacy.erase("title");
  legacy.erase("notes");
  legacy.insert("description", "Old field");
  const auto rejected = codec::todo_snapshot_from_toml(legacy);
  ASSERT_TRUE(is_error(rejected));
  EXPECT_EQ(std::get<codec::CodecError>(rejected).code,
            codec::CodecErrorCode::MissingField);
}

TEST(CoreCodecSnapshotTest, RoundTripsFullTodoAndSerializedText) {
  Todo original("Plan trip", "First line\n旅行\nCafé",
                sample_timestamp_context(std::string("Osaka")),
                Privacy::Protected);
  const auto modified = sample_timestamp_context(std::string("Kyoto"));
  original.set_status(TodoStatus::Completed, modified);
  original.set_priority(TodoPriority::High, modified);
  original.set_due_date(
      DueDate{TimestampContext{Timestamp{std::chrono::seconds{1900000000}},
                               "Asia/Tokyo", std::nullopt}},
      modified);
  original.set_directory_id(sample_id(), modified);
  original.add_subtask("First", modified);
  original.add_subtask("\xE7\xAC\xAC\xE4\xBA\x8C", modified);
  original.set_subtask_completed(1, true, modified);

  const auto snapshot = original.snapshot();
  const auto document = codec::to_toml(snapshot);
  EXPECT_EQ(document.get("title")->value<std::string>(),
            std::optional<std::string>{"Plan trip"});
  EXPECT_EQ(document.get("notes")->value<std::string>(),
            std::optional<std::string>{"First line\n旅行\nCafé"});
  const auto* due = document.get_as<toml::table>("due");
  ASSERT_NE(due, nullptr);
  EXPECT_EQ(due->get("kind")->value<std::string>(),
            std::optional<std::string>{"Datetime"});
  EXPECT_FALSE(due->contains("date"));
  EXPECT_TRUE(document.contains("directory_id"));
  ASSERT_NE(document.get_as<toml::array>("subtasks"), nullptr);
  ASSERT_EQ(document.get_as<toml::array>("subtasks")->size(), 2U);
  const auto serialized = codec::serialize_toml(codec::to_toml(snapshot));
  ASSERT_FALSE(is_error(serialized));
  const auto parsed = codec::parse_toml(value_of(serialized));
  ASSERT_FALSE(is_error(parsed));
  const auto decoded = codec::todo_snapshot_from_toml(value_of(parsed));
  ASSERT_FALSE(is_error(decoded));

  const Todo restored = Todo::restore(value_of(decoded));
  const auto actual = restored.snapshot();
  EXPECT_EQ(actual.id, snapshot.id);
  expect_timestamp_context_eq(actual.created, snapshot.created);
  expect_timestamp_context_eq(actual.modified, snapshot.modified);
  EXPECT_EQ(actual.title, snapshot.title);
  EXPECT_EQ(actual.notes, snapshot.notes);
  EXPECT_EQ(actual.status, snapshot.status);
  EXPECT_EQ(actual.priority, snapshot.priority);
  expect_due_date_eq(actual.due_date, snapshot.due_date);
  EXPECT_EQ(actual.directory_id, snapshot.directory_id);
  EXPECT_EQ(actual.privacy, snapshot.privacy);
  ASSERT_EQ(actual.subtasks.size(), 2U);
  EXPECT_EQ(actual.subtasks[0].content, snapshot.subtasks[0].content);
  EXPECT_EQ(actual.subtasks[0].completed, snapshot.subtasks[0].completed);
  EXPECT_EQ(actual.subtasks[1].content, snapshot.subtasks[1].content);
  EXPECT_EQ(actual.subtasks[1].completed, snapshot.subtasks[1].completed);
}

TEST(CoreCodecSnapshotTest, DirectoryNameAndIdentitySurviveRoundTrip) {
  const Directory original(DirectoryKind::Todo,
                           "\xE7\xAC\x94\xE8\xAE\xB0\xE5\xBA\x93");
  const auto snapshot = original.snapshot();
  const auto document = codec::to_toml(snapshot);
  EXPECT_EQ(document.get("schema_version")->value<std::int64_t>(),
            std::optional<std::int64_t>{1});
  EXPECT_EQ(document.get("id")->value<std::string>(),
            std::optional<std::string>{snapshot.id.to_string()});
  EXPECT_EQ(document.get("name")->value<std::string>(),
            std::optional<std::string>{original.name()});
  EXPECT_EQ(document.get("kind")->value<std::string>(),
            std::optional<std::string>{"Todo"});
  const auto serialized = codec::serialize_toml(codec::to_toml(snapshot));
  ASSERT_FALSE(is_error(serialized));
  const auto parsed = codec::parse_toml(value_of(serialized));
  ASSERT_FALSE(is_error(parsed));
  const auto decoded = codec::directory_snapshot_from_toml(value_of(parsed));
  ASSERT_FALSE(is_error(decoded));

  const Directory restored = Directory::restore(value_of(decoded));
  EXPECT_EQ(restored.id(), original.id());
  EXPECT_EQ(restored.kind(), DirectoryKind::Todo);
  EXPECT_EQ(restored.name(), original.name());
}

TEST(CoreCodecSnapshotTest, DirectoryLegacyKindRequiresTrustedContext) {
  auto legacy = codec::to_toml(Directory::Snapshot{
      sample_id(), DirectoryKind::Memo, "Legacy notes"});
  legacy.erase("kind");
  const auto untrusted = codec::directory_snapshot_from_toml(legacy);
  ASSERT_TRUE(is_error(untrusted));
  EXPECT_EQ(std::get<codec::CodecError>(untrusted).code,
            codec::CodecErrorCode::MissingField);
  EXPECT_EQ(std::get<codec::CodecError>(untrusted).path, "kind");

  const auto trusted = codec::directory_snapshot_from_toml(
      legacy, DirectoryKind::Memo);
  ASSERT_FALSE(is_error(trusted));
  EXPECT_EQ(value_of(trusted).kind, DirectoryKind::Memo);
  EXPECT_EQ(value_of(trusted).name, "Legacy notes");

  auto explicit_kind = legacy;
  explicit_kind.insert("kind", "Todo");
  const auto conflicting = codec::directory_snapshot_from_toml(
      explicit_kind, DirectoryKind::Memo);
  ASSERT_TRUE(is_error(conflicting));
  EXPECT_EQ(std::get<codec::CodecError>(conflicting).code,
            codec::CodecErrorCode::InvalidField);
  EXPECT_EQ(std::get<codec::CodecError>(conflicting).path, "kind");
  const auto invalid = codec::directory_snapshot_from_toml(
      toml::table{{"schema_version", 1}, {"id", sample_id().to_string()},
                  {"kind", "Archive"}, {"name", "Unknown"}});
  ASSERT_TRUE(is_error(invalid));
  EXPECT_EQ(std::get<codec::CodecError>(invalid).code,
            codec::CodecErrorCode::InvalidField);
  EXPECT_EQ(std::get<codec::CodecError>(invalid).path, "kind");
  auto wrong_type = codec::to_toml(Directory::Snapshot{
      sample_id(), DirectoryKind::Memo, "Wrong type"});
  wrong_type.insert_or_assign("kind", 42);
  const auto invalid_type = codec::directory_snapshot_from_toml(wrong_type);
  ASSERT_TRUE(is_error(invalid_type));
  EXPECT_EQ(std::get<codec::CodecError>(invalid_type).code,
            codec::CodecErrorCode::InvalidField);
  EXPECT_EQ(std::get<codec::CodecError>(invalid_type).path, "kind");
  const auto invalid_context = codec::directory_snapshot_from_toml(
      legacy, static_cast<DirectoryKind>(99));
  ASSERT_TRUE(is_error(invalid_context));
  EXPECT_EQ(std::get<codec::CodecError>(invalid_context).code,
            codec::CodecErrorCode::InvalidField);
  EXPECT_EQ(std::get<codec::CodecError>(invalid_context).path, "kind");
}

TEST(CoreCodecSnapshotTest, EncodesExplicitSchemaWithoutPlaceholderValues) {
  const Todo::Snapshot snapshot{
      sample_id(),
      {Timestamp{std::chrono::seconds{86400}}, "Asia/Tokyo", "Tokyo"},
      {Timestamp{std::chrono::seconds{172800}}, "Europe/Paris", std::nullopt},
      "Review", "First line\n第二行", TodoStatus::Completed, TodoPriority::Medium,
      DueDate{DateContext{{std::chrono::year{2026} / 10 / 25},
                          "Asia/Shanghai", std::nullopt}},
      sample_id(), Privacy::Hidden, {{"Read", true}, {"Reply", false}}};
  const auto expected = codec::parse_toml(R"toml(
schema_version = 1
id = "47183823-2574-4bfd-b411-99ed177d3e43"
title = "Review"
notes = "First line\n第二行"
status = "Completed"
priority = "Medium"
privacy = "Hidden"
directory_id = "47183823-2574-4bfd-b411-99ed177d3e43"

[created]
timestamp = "1970-01-02T00:00:00Z"
timezone = "Asia/Tokyo"
location = "Tokyo"

[modified]
timestamp = "1970-01-03T00:00:00Z"
timezone = "Europe/Paris"

[due]
kind = "Date"
date = "2026-10-25"
timezone = "Asia/Shanghai"

[[subtasks]]
content = "Read"
completed = true

[[subtasks]]
content = "Reply"
completed = false
)toml");
  ASSERT_FALSE(is_error(expected));
  EXPECT_EQ(codec::to_toml(snapshot), value_of(expected));

  const toml::table expected_directory{
      {"schema_version", 1}, {"id", sample_id().to_string()},
      {"kind", "Memo"},
      {"name", "Research"}};
  EXPECT_EQ(codec::to_toml(Directory::Snapshot{sample_id(), DirectoryKind::Memo,
                                               "Research"}),
            expected_directory);
}

TEST(CoreCodecContextTest, DueVariantsEmitOnlyTheirOwnFields) {
  const DueDate date = DateContext{{std::chrono::year{2026} / 10 / 25},
                                   "Asia/Tokyo", std::nullopt};
  const toml::table expected_date{{"kind", "Date"}, {"date", "2026-10-25"},
                                  {"timezone", "Asia/Tokyo"}};
  EXPECT_EQ(codec::to_toml(date), expected_date);

  const DueDate datetime = TimestampContext{
      Timestamp{std::chrono::seconds{86400}}, "Europe/Paris", "Paris"};
  const toml::table expected_datetime{
      {"kind", "Datetime"}, {"timestamp", "1970-01-02T00:00:00Z"},
      {"timezone", "Europe/Paris"}, {"location", "Paris"}};
  EXPECT_EQ(codec::to_toml(datetime), expected_datetime);
}

TEST(CoreCodecSnapshotTest, UsesStableStringsForEveryPriority) {
  const std::vector<std::pair<TodoPriority, std::string>> priorities{
      {TodoPriority::None, "None"}, {TodoPriority::Low, "Low"},
      {TodoPriority::Medium, "Medium"}, {TodoPriority::High, "High"}};
  auto snapshot = Todo("Priority", "", sample_timestamp_context(std::nullopt)).snapshot();
  for (const auto& [priority, text] : priorities) {
    SCOPED_TRACE(text);
    snapshot.priority = priority;
    const auto table = codec::to_toml(snapshot);
    EXPECT_EQ(table["priority"].value<std::string>(),
              std::optional<std::string>{text});
    const auto decoded = codec::todo_snapshot_from_toml(table);
    ASSERT_FALSE(is_error(decoded));
    EXPECT_EQ(value_of(decoded).priority, priority);
  }
}

TEST(CoreCodecErrorTest, RejectsEachMissingRequiredSnapshotField) {
  const auto todo = codec::to_toml(
      Todo("Required fields", "", sample_timestamp_context(std::nullopt)).snapshot());
  for (const auto* field : {"schema_version", "id", "created", "modified",
                             "title", "notes", "status", "priority", "privacy"}) {
    SCOPED_TRACE(field);
    auto incomplete = todo;
    incomplete.erase(field);
    const auto decoded = codec::todo_snapshot_from_toml(incomplete);
    ASSERT_TRUE(is_error(decoded));
    const auto& error = std::get<codec::CodecError>(decoded);
    EXPECT_EQ(error.code, codec::CodecErrorCode::MissingField);
    EXPECT_EQ(error.path, field);
  }

  const auto directory = codec::to_toml(Directory::Snapshot{
      sample_id(), DirectoryKind::Memo, "Work"});
  for (const auto* field : {"schema_version", "id", "kind", "name"}) {
    SCOPED_TRACE(field);
    auto incomplete = directory;
    incomplete.erase(field);
    const auto decoded = codec::directory_snapshot_from_toml(incomplete);
    ASSERT_TRUE(is_error(decoded));
    const auto& error = std::get<codec::CodecError>(decoded);
    EXPECT_EQ(error.code, codec::CodecErrorCode::MissingField);
    EXPECT_EQ(error.path, field);
  }
}

TEST(CoreCodecErrorTest, RejectsMalformedTomlAndMissingRequiredFields) {
  EXPECT_TRUE(is_error(codec::parse_toml("title = [")));
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(toml::table{})));
  EXPECT_TRUE(is_error(codec::directory_snapshot_from_toml(toml::table{})));
  EXPECT_TRUE(is_error(codec::id_from_toml(toml::value<std::string>{"not-a-uuid"})));
  EXPECT_TRUE(is_error(codec::privacy_from_toml(
      toml::value<std::string>{"not-a-privacy"})));
  EXPECT_TRUE(is_error(codec::tag_from_toml(toml::value<std::int64_t>{42})));
  EXPECT_TRUE(is_error(codec::timestamp_context_from_toml(toml::value<bool>{true})));
  EXPECT_TRUE(is_error(codec::date_context_from_toml(toml::value<bool>{true})));
  EXPECT_TRUE(is_error(codec::due_date_from_toml(toml::value<bool>{true})));
}

TEST(CoreCodecErrorTest, RejectsInvalidDateTimestampAndSchemaVersion) {
  auto invalid_timestamp =
      codec::to_toml(sample_timestamp_context(std::nullopt));
  invalid_timestamp.insert_or_assign("timestamp", "not-a-timestamp");
  EXPECT_TRUE(is_error(codec::timestamp_context_from_toml(invalid_timestamp)));

  auto invalid_date = codec::to_toml(
      DateContext{{std::chrono::year{2025} / 3 / 4}, "UTC", std::nullopt});
  invalid_date.insert_or_assign("date", "2025-02-30");
  EXPECT_TRUE(is_error(codec::date_context_from_toml(invalid_date)));

  auto empty_timezone =
      codec::to_toml(sample_timestamp_context(std::nullopt));
  empty_timezone.insert_or_assign("timezone", "");
  EXPECT_TRUE(is_error(codec::timestamp_context_from_toml(empty_timezone)));

  Todo todo("Versioned", "", sample_timestamp_context(std::nullopt));
  auto unsupported = codec::to_toml(todo.snapshot());
  unsupported.insert_or_assign("schema_version", 999);
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(unsupported)));

  auto wrong_type = codec::to_toml(todo.snapshot());
  wrong_type.insert_or_assign("id", 42);
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(wrong_type)));

  for (const auto* field : {"title", "notes"}) {
    SCOPED_TRACE(field);
    auto invalid_text = codec::to_toml(todo.snapshot());
    invalid_text.insert_or_assign(field, 42);
    const auto decoded = codec::todo_snapshot_from_toml(invalid_text);
    ASSERT_TRUE(is_error(decoded));
    EXPECT_EQ(std::get<codec::CodecError>(decoded).code,
              codec::CodecErrorCode::InvalidField);
    EXPECT_EQ(std::get<codec::CodecError>(decoded).path, field);
  }

  auto invalid_status = codec::to_toml(todo.snapshot());
  invalid_status.insert_or_assign("status", "InProgress");
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(invalid_status)));

  auto invalid_priority = codec::to_toml(todo.snapshot());
  invalid_priority.insert_or_assign("priority", "Urgent");
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(invalid_priority)));

  auto invalid_due_kind = codec::to_toml(todo.snapshot());
  invalid_due_kind.insert_or_assign(
      "due", toml::table{{"kind", "DateTime"}, {"date", "2025-03-04"},
                         {"timezone", "UTC"}});
  EXPECT_TRUE(is_error(codec::todo_snapshot_from_toml(invalid_due_kind)));

  auto directory = codec::to_toml(
      Directory(DirectoryKind::Memo, "Projects").snapshot());
  directory.insert_or_assign("schema_version", 2);
  EXPECT_TRUE(is_error(codec::directory_snapshot_from_toml(directory)));
}

// Independent fixtures keep schema checks separate from the Markdown encoder.
std::string markdown_metadata(bool is_log) {
  std::string text = R"toml(schema_version = 1
id = "47183823-2574-4bfd-b411-99ed177d3e43"
privacy = "Hidden"
title = "旅の記録"
tags = ["旅行", "", "notes", "旅行"]
created = { timestamp = "1970-01-02T00:00:00.123456Z", timezone = "Asia/Tokyo", location = "東京" }
modified = { timestamp = "1970-01-03T00:00:00Z", timezone = "Europe/Paris", location = "Paris" }
)toml";
  text += is_log
      ? "log_date = { date = \"2024-02-29\", timezone = \"Asia/Shanghai\", location = \"上海\" }\n"
      : "directory_id = \"27183823-2574-4bfd-b411-99ed177d3e43\"\n";
  return text;
}

Log::Snapshot markdown_log() {
  return {sample_id(),
          {Timestamp{std::chrono::duration_cast<Timestamp::duration>(
              std::chrono::seconds{86400} +
              std::chrono::microseconds{123456})}, "Asia/Tokyo", "東京"},
          {Timestamp{std::chrono::seconds{172800}}, "Europe/Paris", "Paris"},
          "旅の記録", "# 本文\n\nText\r\n+++\nlast line",
          {{std::chrono::year{2024} / 2 / 29}, "Asia/Shanghai", "上海"},
          {{"旅行"}, {""}, {"notes"}, {"旅行"}}, Privacy::Hidden};
}

Memo::Snapshot markdown_memo() {
  const auto log = markdown_log();
  return {log.id, log.created, log.modified, log.title, log.content, log.tags,
          *Id::parse("27183823-2574-4bfd-b411-99ed177d3e43"), log.privacy};
}

template <typename Snapshot>
void expect_markdown_common_eq(const Snapshot& actual, const Snapshot& expected) {
  EXPECT_EQ(actual.id, expected.id);
  expect_timestamp_context_eq(actual.created, expected.created);
  expect_timestamp_context_eq(actual.modified, expected.modified);
  EXPECT_EQ(actual.title, expected.title);
  EXPECT_EQ(actual.content, expected.content);
  EXPECT_EQ(actual.tags, expected.tags);
  EXPECT_EQ(actual.privacy, expected.privacy);
}

void expect_log_eq(const Log::Snapshot& actual, const Log::Snapshot& expected) {
  expect_markdown_common_eq(actual, expected);
  expect_date_context_eq(actual.log_date, expected.log_date);
}

void expect_memo_eq(const Memo::Snapshot& actual, const Memo::Snapshot& expected) {
  expect_markdown_common_eq(actual, expected);
  EXPECT_EQ(actual.directory_id, expected.directory_id);
}

void expect_markdown_error(bool is_log, const std::string& document,
                           codec::CodecErrorCode code) {
  const auto check_error = [code](const codec::CodecError& error) {
    EXPECT_EQ(error.code, code);
    EXPECT_FALSE(error.message.empty());
    if (code == codec::CodecErrorCode::MissingField ||
        code == codec::CodecErrorCode::InvalidField ||
        code == codec::CodecErrorCode::UnknownField ||
        code == codec::CodecErrorCode::UnsupportedSchemaVersion) {
      EXPECT_FALSE(error.path.empty());
    }
  };
  if (is_log) {
    const auto result = codec::log_snapshot_from_markdown(document);
    ASSERT_TRUE(is_error(result));
    check_error(std::get<codec::CodecError>(result));
  } else {
    const auto result = codec::memo_snapshot_from_markdown(document);
    ASSERT_TRUE(is_error(result));
    check_error(std::get<codec::CodecError>(result));
  }
}

TEST(CoreCodecMarkdownTest, DecodesIndependentFixturesAndRestoresAllFields) {
  const auto log = markdown_log();
  const auto decoded_log = codec::log_snapshot_from_markdown(
      "+++\n" + markdown_metadata(true) + "+++\n" + log.content);
  ASSERT_FALSE(is_error(decoded_log));
  expect_log_eq(value_of(decoded_log), log);
  expect_log_eq(Log::restore(value_of(decoded_log)).snapshot(), log);

  const auto memo = markdown_memo();
  const auto decoded_memo = codec::memo_snapshot_from_markdown(
      "+++\n" + markdown_metadata(false) + "+++\n" + memo.content);
  ASSERT_FALSE(is_error(decoded_memo));
  expect_memo_eq(value_of(decoded_memo), memo);
  expect_memo_eq(Memo::restore(value_of(decoded_memo)).snapshot(), memo);
}

TEST(CoreCodecMarkdownTest, RoundTripsFullSnapshotsWithLfFraming) {
  const auto log = markdown_log();
  const auto encoded_log = codec::serialize_markdown(log);
  ASSERT_FALSE(is_error(encoded_log));
  EXPECT_TRUE(value_of(encoded_log).starts_with("+++\n"));
  EXPECT_TRUE(value_of(encoded_log).ends_with("\n+++\n" + log.content));
  const auto decoded_log = codec::log_snapshot_from_markdown(value_of(encoded_log));
  ASSERT_FALSE(is_error(decoded_log));
  expect_log_eq(Log::restore(value_of(decoded_log)).snapshot(), log);

  const auto memo = markdown_memo();
  const auto encoded_memo = codec::serialize_markdown(memo);
  ASSERT_FALSE(is_error(encoded_memo));
  EXPECT_TRUE(value_of(encoded_memo).starts_with("+++\n"));
  EXPECT_TRUE(value_of(encoded_memo).ends_with("\n+++\n" + memo.content));
  const auto decoded_memo = codec::memo_snapshot_from_markdown(value_of(encoded_memo));
  ASSERT_FALSE(is_error(decoded_memo));
  expect_memo_eq(Memo::restore(value_of(decoded_memo)).snapshot(), memo);
}

TEST(CoreCodecMarkdownTest, PreservesEveryBodyByteAcrossFramingLineEndings) {
  const std::vector<std::string> bodies{
      "", "\n", " \t\r\n\n ", "no final newline", "+++", "+++\n+++\r\n",
      "\n# 見出し\r\n```toml\ncontent = 'body'\r\n+++\n```\n"
      "| a | b |\n|---|---|\n| 1 | 2 |\n\tend  ",
      std::string("a\0b\r\n", 5)};
  for (const auto& body : bodies) {
    SCOPED_TRACE(body);
    auto log = markdown_log();
    auto memo = markdown_memo();
    log.content = body;
    memo.content = body;
    const auto encoded_log = codec::serialize_markdown(log);
    const auto encoded_memo = codec::serialize_markdown(memo);
    ASSERT_FALSE(is_error(encoded_log));
    ASSERT_FALSE(is_error(encoded_memo));
    const auto roundtrip_log = codec::log_snapshot_from_markdown(value_of(encoded_log));
    const auto roundtrip_memo = codec::memo_snapshot_from_markdown(value_of(encoded_memo));
    ASSERT_FALSE(is_error(roundtrip_log));
    ASSERT_FALSE(is_error(roundtrip_memo));
    EXPECT_EQ(value_of(roundtrip_log).content, body);
    EXPECT_EQ(value_of(roundtrip_memo).content, body);

    for (const auto* opening : {"+++\n", "+++\r\n"}) {
      for (const auto* closing : {"+++\n", "+++\r\n"}) {
        for (const bool is_log : {true, false}) {
          auto metadata = markdown_metadata(is_log);
          // Alternate metadata line endings independently of delimiter endings.
          if (std::string_view{opening} == "+++\r\n") {
            for (std::size_t pos = 0; (pos = metadata.find('\n', pos)) !=
                 std::string::npos; pos += 2) {
              metadata.insert(pos, 1, '\r');
            }
          }
          const auto document = opening + metadata + closing + body;
          if (is_log) {
            const auto result = codec::log_snapshot_from_markdown(document);
            ASSERT_FALSE(is_error(result));
            EXPECT_EQ(value_of(result).content, body);
          } else {
            const auto result = codec::memo_snapshot_from_markdown(document);
            ASSERT_FALSE(is_error(result));
            EXPECT_EQ(value_of(result).content, body);
          }
        }
      }
    }
  }
}

TEST(CoreCodecMarkdownTest, RoundTripsEmptyValuesPrivacyAndOmittedOptionals) {
  for (const auto privacy : {Privacy::Normal, Privacy::Protected, Privacy::Hidden}) {
    auto log = markdown_log();
    auto memo = markdown_memo();
    log.privacy = memo.privacy = privacy;
    log.title = memo.title = "";
    log.content = memo.content = "";
    log.tags.clear();
    memo.tags.clear();
    log.created.location.reset();
    log.modified.location.reset();
    log.log_date.location.reset();
    memo.created.location.reset();
    memo.modified.location.reset();
    memo.directory_id.reset();
    const auto encoded_log = codec::serialize_markdown(log);
    const auto encoded_memo = codec::serialize_markdown(memo);
    ASSERT_FALSE(is_error(encoded_log));
    ASSERT_FALSE(is_error(encoded_memo));
    for (const auto* text : {&value_of(encoded_log), &value_of(encoded_memo)}) {
      EXPECT_EQ(text->find("location"), std::string::npos);
      EXPECT_EQ(text->find("directory_id"), std::string::npos);
      const auto end = text->find("\n+++\n", 4);
      ASSERT_NE(end, std::string::npos);
      const auto metadata = codec::parse_toml(text->substr(4, end - 4));
      ASSERT_FALSE(is_error(metadata));
      ASSERT_NE(value_of(metadata).get_as<toml::array>("tags"), nullptr);
      EXPECT_TRUE(value_of(metadata).get_as<toml::array>("tags")->empty());
      EXPECT_FALSE(value_of(metadata).contains("content"));
    }
    const auto decoded_log = codec::log_snapshot_from_markdown(value_of(encoded_log));
    const auto decoded_memo = codec::memo_snapshot_from_markdown(value_of(encoded_memo));
    ASSERT_FALSE(is_error(decoded_log));
    ASSERT_FALSE(is_error(decoded_memo));
    expect_log_eq(value_of(decoded_log), log);
    expect_memo_eq(value_of(decoded_memo), memo);
  }
}

TEST(CoreCodecMarkdownTest, EscapesDelimiterLinesInsideMetadataStrings) {
  for (const auto& collision : {std::string("before\n+++\nafter"),
                                std::string("before\r\n+++\r\nafter")}) {
    auto log = markdown_log();
    auto memo = markdown_memo();
    log.title = memo.title = collision;
    log.tags = memo.tags = {{collision}, {"quotes \"\"\" and ''' \\"}};
    log.created.location = memo.created.location = collision;
    log.modified.location = memo.modified.location = collision;
    log.log_date.location = collision;
    const auto encoded_log = codec::serialize_markdown(log);
    const auto encoded_memo = codec::serialize_markdown(memo);
    ASSERT_FALSE(is_error(encoded_log));
    ASSERT_FALSE(is_error(encoded_memo));
    const auto decoded_log = codec::log_snapshot_from_markdown(value_of(encoded_log));
    const auto decoded_memo = codec::memo_snapshot_from_markdown(value_of(encoded_memo));
    ASSERT_FALSE(is_error(decoded_log));
    ASSERT_FALSE(is_error(decoded_memo));
    expect_log_eq(value_of(decoded_log), log);
    expect_memo_eq(value_of(decoded_memo), memo);
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsMalformedFramingAndTomlRecoverably) {
  for (const bool is_log : {true, false}) {
    const auto metadata = markdown_metadata(is_log);
    const std::vector<std::string> malformed{
        "", "+++", "+++\r", "---\n" + metadata + "---\n",
        "\n+++\n" + metadata + "+++\n",
        "preamble\n+++\n" + metadata + "+++\n",
        "\xEF\xBB\xBF+++\n" + metadata + "+++\n",
        " +++\n" + metadata + "+++\n",
        "+++ \n" + metadata + "+++\n",
        "+++\t\r\n" + metadata + "+++\n",
        "+++\r" + metadata + "+++\n",
        "+++\n" + metadata,
        "+++\n" + metadata + "+++",
        "+++\n" + metadata + "+++\r",
        "+++\n" + metadata + "+++ \n",
        "+++\n" + metadata + " +++\n",
        "+++\n" + metadata + "+++\t\r\n"};
    for (const auto& document : malformed) {
      SCOPED_TRACE(document);
      expect_markdown_error(is_log, document, codec::CodecErrorCode::MalformedFrontMatter);
    }
    expect_markdown_error(is_log, "+++\ntitle = [\n+++\nbody",
                          codec::CodecErrorCode::MalformedToml);
    // The first exact delimiter wins even inside a manually written multiline string.
    expect_markdown_error(is_log, "+++\ntitle = '''before\n+++\nafter'''\n+++\n",
                          codec::CodecErrorCode::MalformedToml);
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsEveryMissingAndMistypedRequiredField) {
  for (const bool is_log : {true, false}) {
    const auto fixture = codec::parse_toml(markdown_metadata(is_log));
    ASSERT_FALSE(is_error(fixture));
    std::vector<std::string> required{
        "schema_version", "id", "privacy", "title", "tags", "created", "modified"};
    if (is_log) required.push_back("log_date");
    for (const auto& key : required) {
      SCOPED_TRACE(key);
      auto missing = value_of(fixture);
      missing.erase(key);
      const auto missing_text = codec::serialize_toml(missing);
      ASSERT_FALSE(is_error(missing_text));
      expect_markdown_error(is_log, "+++\n" + value_of(missing_text) + "\n+++\n",
                            codec::CodecErrorCode::MissingField);
      auto mistyped = value_of(fixture);
      mistyped.insert_or_assign(key, false);
      const auto mistyped_text = codec::serialize_toml(mistyped);
      ASSERT_FALSE(is_error(mistyped_text));
      expect_markdown_error(is_log, "+++\n" + value_of(mistyped_text) + "\n+++\n",
                            codec::CodecErrorCode::InvalidField);
    }
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsCoercibleNonIntegerSchemaVersions) {
  for (const bool is_log : {true, false}) {
    for (const auto* version : {"true", "1.0"}) {
      SCOPED_TRACE(version);
      auto metadata = markdown_metadata(is_log);
      metadata.replace(0, std::string_view{"schema_version = 1"}.size(),
                       std::string{"schema_version = "} + version);
      expect_markdown_error(is_log, "+++\n" + metadata + "+++\n",
                            codec::CodecErrorCode::InvalidField);
    }
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsUnknownFieldsAndContentMetadata) {
  for (const bool is_log : {true, false}) {
    const std::vector<std::string> extras{
        "unknown = 42\n", "content = 'must not shadow body'\n",
        "created.extra = 'unknown'\n", "modified.extra = 'unknown'\n",
        is_log ? "log_date.extra = 'unknown'\n" : "log_date = {}\n",
        is_log ? "directory_id = '27183823-2574-4bfd-b411-99ed177d3e43'\n"
               : "status = 'Pending'\n"};
    for (const auto& extra : extras) {
      SCOPED_TRACE(extra);
      // Mutate inline context tables directly to keep the TOML itself valid.
      const auto fixture = codec::parse_toml(markdown_metadata(is_log));
      ASSERT_FALSE(is_error(fixture));
      auto table = value_of(fixture);
      const auto dot = extra.find('.');
      if (dot != std::string::npos && dot < extra.find('=')) {
        table.get_as<toml::table>(extra.substr(0, dot))->insert("extra", "unknown");
      } else {
        const auto added = codec::parse_toml(extra);
        ASSERT_FALSE(is_error(added));
        for (const auto& [key, node] : value_of(added)) table.insert(key.str(), node);
      }
      const auto text = codec::serialize_toml(table);
      ASSERT_FALSE(is_error(text));
      expect_markdown_error(is_log, "+++\n" + value_of(text) + "\n+++\nbody",
                            codec::CodecErrorCode::UnknownField);
    }
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsInvalidMetadataValuesAndContextFields) {
  for (const bool is_log : {true, false}) {
    const auto fixture = codec::parse_toml(markdown_metadata(is_log));
    ASSERT_FALSE(is_error(fixture));
    const std::vector<std::pair<std::string, std::string>> replacements{
        {"id", "not-a-uuid"}, {"id", "00000000-0000-0000-0000-000000000000"},
        {"privacy", "Secret"}, {"privacy", "hidden"}, {"tags", "not-an-array"},
        {"schema_version", "1"}};
    for (const auto& [key, value] : replacements) {
      SCOPED_TRACE(key + "=" + value);
      auto table = value_of(fixture);
      table.insert_or_assign(key, value);
      const auto text = codec::serialize_toml(table);
      ASSERT_FALSE(is_error(text));
      expect_markdown_error(is_log, "+++\n" + value_of(text) + "\n+++\n",
                            codec::CodecErrorCode::InvalidField);
    }
    for (const int version : {0, 2, 999}) {
      auto table = value_of(fixture);
      table.insert_or_assign("schema_version", version);
      const auto text = codec::serialize_toml(table);
      ASSERT_FALSE(is_error(text));
      expect_markdown_error(is_log, "+++\n" + value_of(text) + "\n+++\n",
                            codec::CodecErrorCode::UnsupportedSchemaVersion);
    }
    auto invalid_tags = value_of(fixture);
    invalid_tags.insert_or_assign("tags", toml::array{"valid", 42});
    const auto tags_text = codec::serialize_toml(invalid_tags);
    ASSERT_FALSE(is_error(tags_text));
    expect_markdown_error(is_log, "+++\n" + value_of(tags_text) + "\n+++\n",
                          codec::CodecErrorCode::InvalidField);

    std::vector<std::string> contexts{"created", "modified"};
    if (is_log) contexts.push_back("log_date");
    for (const auto& context : contexts) {
      const std::string value_key = context == "log_date" ? "date" : "timestamp";
      for (const auto& key : {value_key, std::string("timezone"), std::string("location")}) {
        for (const int mutation : {0, 1, 2}) {
          SCOPED_TRACE(context + "." + key + ":" + std::to_string(mutation));
          auto table = value_of(fixture);
          auto* nested = table.get_as<toml::table>(context);
          auto expected = codec::CodecErrorCode::InvalidField;
          if (mutation == 0) {
            if (key == "location") continue;  // Optional, covered by round trips.
            nested->erase(key);
            expected = codec::CodecErrorCode::MissingField;
          } else if (mutation == 1) {
            nested->insert_or_assign(key, 42);
          } else {
            if (key == "location") continue;  // Empty locations are permitted.
            nested->insert_or_assign(key, key == "date" ? "2025-02-30" : "");
          }
          const auto text = codec::serialize_toml(table);
          ASSERT_FALSE(is_error(text));
          expect_markdown_error(is_log, "+++\n" + value_of(text) + "\n+++\n", expected);
        }
      }
    }
    if (!is_log) {
      for (const auto* invalid : {"not-a-uuid", ""}) {
        auto table = value_of(fixture);
        table.insert_or_assign("directory_id", invalid);
        const auto text = codec::serialize_toml(table);
        ASSERT_FALSE(is_error(text));
        expect_markdown_error(false, "+++\n" + value_of(text) + "\n+++\n",
                              codec::CodecErrorCode::InvalidField);
      }
      auto table = value_of(fixture);
      table.insert_or_assign("directory_id", 42);
      const auto text = codec::serialize_toml(table);
      ASSERT_FALSE(is_error(text));
      expect_markdown_error(false, "+++\n" + value_of(text) + "\n+++\n",
                            codec::CodecErrorCode::InvalidField);
    }
  }
}

TEST(CoreCodecMarkdownErrorTest, RejectsSignedDateAndTimestampFields) {
  for (const bool is_log : {true, false}) {
    auto metadata = markdown_metadata(is_log);
    const std::string_view timestamp = "1970-01-02T00:00:00.123456Z";
    const auto position = metadata.find(timestamp);
    ASSERT_NE(position, std::string::npos);
    metadata.replace(position, timestamp.size(), "1970-01-02T-1:00:00Z");
    expect_markdown_error(is_log, "+++\n" + metadata + "+++\n",
                          codec::CodecErrorCode::InvalidField);
  }

  auto metadata = markdown_metadata(true);
  const std::string_view date = "2024-02-29";
  const auto position = metadata.find(date);
  ASSERT_NE(position, std::string::npos);
  metadata.replace(position, date.size(), "-001-01-02");
  expect_markdown_error(true, "+++\n" + metadata + "+++\n",
                        codec::CodecErrorCode::InvalidField);
}

TEST(CoreCodecMarkdownErrorTest, EncodingInvalidSnapshotsReturnsErrors) {
  auto log = markdown_log();
  auto memo = markdown_memo();
  log.privacy = memo.privacy = static_cast<Privacy>(999);
  EXPECT_TRUE(is_error(codec::serialize_markdown(log)));
  EXPECT_TRUE(is_error(codec::serialize_markdown(memo)));
  log = markdown_log();
  log.log_date.date = std::chrono::year{2025} / 2 / 30;
  EXPECT_TRUE(is_error(codec::serialize_markdown(log)));
  log = markdown_log();
  memo = markdown_memo();
  log.created.timezone.clear();
  memo.modified.timezone.clear();
  EXPECT_TRUE(is_error(codec::serialize_markdown(log)));
  EXPECT_TRUE(is_error(codec::serialize_markdown(memo)));
}

}  // namespace
