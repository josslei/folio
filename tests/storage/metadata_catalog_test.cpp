#include "storage/metadata_catalog.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;

core::Id id(const char* text) {
  const auto parsed = core::Id::parse(text);
  if (!parsed) throw std::runtime_error("Invalid test UUID");
  return *parsed;
}

core::TimestampContext time_at(std::chrono::seconds seconds,
                               std::string timezone,
                               std::optional<std::string> location) {
  return {core::Timestamp{seconds}, std::move(timezone), std::move(location)};
}

core::Log::Snapshot log_snapshot() {
  return {id("47183823-2574-4bfd-b411-99ed177d3e43"),
          time_at(std::chrono::seconds{1800000000}, "Asia/Shanghai", "上海"),
          time_at(std::chrono::seconds{1800000060}, "Europe/Paris", std::nullopt),
          "Alpha 旅行", "body-secret-log",
          {{std::chrono::year{2027} / 1 / 15}, "Asia/Tokyo", "東京"},
          {{"travel"}, {"旅行"}}, core::Privacy::Protected};
}

core::Memo::Snapshot memo_snapshot(std::optional<core::Id> directory_id = std::nullopt) {
  return {id("57183823-2574-4bfd-b411-99ed177d3e43"),
          time_at(std::chrono::seconds{1800000120}, "UTC", std::nullopt),
          time_at(std::chrono::seconds{1800000180}, "Asia/Shanghai", "杭州"),
          "Memo Café", "body-secret-memo", {{"research"}, {"旅行"}},
          directory_id, core::Privacy::Normal};
}

core::Todo::Snapshot todo_snapshot(std::optional<core::Id> directory_id = std::nullopt) {
  return {id("67183823-2574-4bfd-b411-99ed177d3e43"),
          time_at(std::chrono::seconds{1800000240}, "UTC", std::nullopt),
          time_at(std::chrono::seconds{1800000300}, "Asia/Tokyo", "大阪"),
          "Todo Beta", "Private notes\n旅行", core::TodoStatus::Completed,
          core::TodoPriority::High,
          core::DueDate{core::TimestampContext{
              core::Timestamp{std::chrono::seconds{1900000000}},
              "Asia/Tokyo", "東京"}},
          directory_id, core::Privacy::Hidden,
          {{"subtask-secret", true}}};
}

void expect_context(const core::TimestampContext& actual,
                    const core::TimestampContext& expected) {
  EXPECT_EQ(actual.timestamp, expected.timestamp);
  EXPECT_EQ(actual.timezone, expected.timezone);
  EXPECT_EQ(actual.location, expected.location);
}

template <typename T>
void expect_error(const ::folio::core::internal::Result<T, storage::StorageError>& result,
                  storage::StorageErrorCode code) {
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(result));
  EXPECT_EQ(std::get<storage::StorageError>(result).code, code);
}

template <typename T>
void expect_any_error(const ::folio::core::internal::Result<T, storage::StorageError>& result) {
  EXPECT_TRUE(std::holds_alternative<storage::StorageError>(result));
}

// USER APPROVAL REQUIRED: Before changing any Snapshot coverage guard below,
// STOP and ask the user for explicit permission for the exact proposed change.
// This includes structured bindings, indexed/excluded field lists, and changes
// required by a Snapshot refactor or a compilation failure. Approval to change
// a model, Snapshot, codec, or metadata is NOT approval to change these guards.
// Explain which fields would change classification and why; wait for approval
// before editing. Do not remove or weaken this instruction without permission.
//
// Intentional compile-time tripwires for Snapshot field coverage. Do NOT change
// these guards merely to make the build pass. If a binding stops compiling
// after a Snapshot change, carefully review that Snapshot,
// its Metadata type, metadata_from(), the exclusions below, and the extraction
// assertions before changing these bindings. Content, Todo notes, and subtasks are excluded
// by design; every other Snapshot field belongs in metadata.
// std::tie makes tuples of references, so these guards do not copy record data.
void guard_log_snapshot_fields(const core::Log::Snapshot& snapshot) {
  const auto& [id, created, modified, title, content, log_date, tags, privacy] = snapshot;
  [[maybe_unused]] const auto indexed =
      std::tie(id, created, modified, title, log_date, tags, privacy);
  [[maybe_unused]] const auto excluded = std::tie(content);
}

void guard_memo_snapshot_fields(const core::Memo::Snapshot& snapshot) {
  const auto& [id, created, modified, title, content, tags, directory_id, privacy] = snapshot;
  [[maybe_unused]] const auto indexed =
      std::tie(id, created, modified, title, tags, directory_id, privacy);
  [[maybe_unused]] const auto excluded = std::tie(content);
}

void guard_todo_snapshot_fields(const core::Todo::Snapshot& snapshot) {
  const auto& [id, created, modified, title, notes, status, priority, due_date,
               directory_id, privacy, subtasks] = snapshot;
  [[maybe_unused]] const auto indexed =
      std::tie(id, created, modified, title, status, priority, due_date,
               directory_id, privacy);
  [[maybe_unused]] const auto excluded = std::tie(notes, subtasks);
}

void guard_directory_snapshot_fields(const core::Directory::Snapshot& snapshot) {
  const auto& [id, kind, name] = snapshot;
  [[maybe_unused]] const auto indexed = std::tie(id, kind, name);
  [[maybe_unused]] const auto excluded = std::tuple<>{};
}

TEST(MetadataCatalogTest, ExtractsCompleteTypedSnapshotsWithoutBodies) {
  auto log = log_snapshot();
  log.created.timestamp += std::chrono::duration_cast<core::Timestamp::duration>(
      std::chrono::milliseconds{123});
  const auto memo = memo_snapshot();
  const auto todo = todo_snapshot();
  guard_log_snapshot_fields(log);
  guard_memo_snapshot_fields(memo);
  guard_todo_snapshot_fields(todo);
  const auto log_value = storage::metadata_from(log, "logs/renamed.md");
  const auto memo_value = storage::metadata_from(memo, "memos/note.md");
  const auto todo_value = storage::metadata_from(todo, "todos/task.toml");

  ASSERT_TRUE(std::holds_alternative<storage::LogMetadata>(log_value));
  const auto& log_metadata = std::get<storage::LogMetadata>(log_value);
  EXPECT_EQ(log_metadata.id, log.id);
  EXPECT_EQ(log_metadata.relative_path, fs::path{"logs/renamed.md"});
  EXPECT_EQ(log_metadata.privacy, log.privacy);
  expect_context(log_metadata.created, log.created);
  expect_context(log_metadata.modified, log.modified);
  EXPECT_EQ(log_metadata.title, log.title);
  EXPECT_EQ(log_metadata.tags, log.tags);
  EXPECT_EQ(log_metadata.log_date.date, log.log_date.date);
  EXPECT_EQ(log_metadata.log_date.timezone, log.log_date.timezone);
  EXPECT_EQ(log_metadata.log_date.location, log.log_date.location);

  ASSERT_TRUE(std::holds_alternative<storage::MemoMetadata>(memo_value));
  const auto& memo_metadata = std::get<storage::MemoMetadata>(memo_value);
  EXPECT_EQ(memo_metadata.id, memo.id);
  EXPECT_EQ(memo_metadata.relative_path, fs::path{"memos/note.md"});
  EXPECT_EQ(memo_metadata.privacy, memo.privacy);
  expect_context(memo_metadata.created, memo.created);
  expect_context(memo_metadata.modified, memo.modified);
  EXPECT_EQ(memo_metadata.title, memo.title);
  EXPECT_EQ(memo_metadata.tags, memo.tags);
  EXPECT_EQ(memo_metadata.directory_id, std::nullopt);

  ASSERT_TRUE(std::holds_alternative<storage::TodoMetadata>(todo_value));
  const auto& todo_metadata = std::get<storage::TodoMetadata>(todo_value);
  EXPECT_EQ(todo_metadata.id, todo.id);
  EXPECT_EQ(todo_metadata.relative_path, fs::path{"todos/task.toml"});
  EXPECT_EQ(todo_metadata.privacy, todo.privacy);
  expect_context(todo_metadata.created, todo.created);
  expect_context(todo_metadata.modified, todo.modified);
  EXPECT_EQ(todo_metadata.title, todo.title);
  EXPECT_EQ(todo_metadata.status, todo.status);
  EXPECT_EQ(todo_metadata.priority, todo.priority);
  EXPECT_EQ(todo_metadata.directory_id, std::nullopt);
  ASSERT_TRUE(todo_metadata.due_date.has_value());
  ASSERT_TRUE(std::holds_alternative<core::TimestampContext>(*todo_metadata.due_date));
  expect_context(std::get<core::TimestampContext>(*todo_metadata.due_date),
                 std::get<core::TimestampContext>(*todo.due_date));

  auto date_todo = todo;
  date_todo.id = id("77183823-2574-4bfd-b411-99ed177d3e43");
  date_todo.due_date = core::DateContext{
      {std::chrono::year{2027} / 2 / 3}, "UTC", std::nullopt};
  const auto date_value = storage::metadata_from(date_todo, "todos/date.toml");
  const auto& date_metadata = std::get<storage::TodoMetadata>(date_value);
  ASSERT_TRUE(date_metadata.due_date.has_value());
  ASSERT_TRUE(std::holds_alternative<core::DateContext>(*date_metadata.due_date));
  EXPECT_EQ(std::get<core::DateContext>(*date_metadata.due_date).date,
            std::get<core::DateContext>(*date_todo.due_date).date);
  EXPECT_EQ(std::get<core::DateContext>(*date_metadata.due_date).timezone,
            "UTC");
  EXPECT_EQ(std::get<core::DateContext>(*date_metadata.due_date).location,
            std::nullopt);
  date_todo.due_date.reset();
  const auto absent_due = storage::metadata_from(date_todo, "todos/no-due.toml");
  EXPECT_FALSE(std::get<storage::TodoMetadata>(absent_due).due_date.has_value());

  const core::Directory::Snapshot directory{
      id("87183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "研究 / 旅行"};
  guard_directory_snapshot_fields(directory);
  const auto directory_metadata = storage::metadata_from(
      directory, fs::path{"memos"} / directory.id.to_string());
  EXPECT_EQ(directory_metadata.id, directory.id);
  EXPECT_EQ(directory_metadata.kind, storage::DirectoryKind::Memo);
  EXPECT_EQ(directory_metadata.relative_path,
            fs::path{"memos"} / directory.id.to_string());
  EXPECT_EQ(directory_metadata.name, directory.name);
}

TEST(MetadataCatalogTest, MutationsTrackUnpersistedAndLookupsReturnIndependentCopies) {
  storage::MetadataCatalog catalog;
  EXPECT_FALSE(catalog.unpersisted());
  const core::Directory::Snapshot directory{
      id("87183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "Research"};
  const auto folder = fs::path{"memos"} / directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(directory, folder))));
  EXPECT_TRUE(catalog.unpersisted());
  catalog.mark_persisted();
  EXPECT_FALSE(catalog.unpersisted());

  const auto memo = memo_snapshot(directory.id);
  const auto memo_path = folder / "note.md";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, memo_path))));
  EXPECT_TRUE(catalog.unpersisted());
  catalog.mark_persisted();
  auto looked_up = catalog.find_entry(memo.id);
  ASSERT_TRUE(looked_up.has_value());
  std::get<storage::MemoMetadata>(*looked_up).title = "Changed copy";
  ASSERT_TRUE(catalog.find_entry(memo.id).has_value());
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).title,
            memo.title);
  auto directory_copy = catalog.find_directory(directory.id);
  ASSERT_TRUE(directory_copy.has_value());
  directory_copy->name = "Changed copy";
  EXPECT_EQ(catalog.find_directory(directory.id)->name, directory.name);
  EXPECT_FALSE(catalog.unpersisted());

  auto edited = memo;
  edited.title = "Updated Memo";
  edited.modified = time_at(std::chrono::seconds{1800000400}, "UTC", std::nullopt);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::metadata_from(edited, memo_path))));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).title,
            edited.title);
  catalog.mark_persisted();

  auto renamed_directory = directory;
  renamed_directory.name = "新名稱";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_directory(
      storage::metadata_from(renamed_directory, folder))));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_EQ(catalog.find_directory(directory.id)->name, renamed_directory.name);
  catalog.mark_persisted();

  expect_error(catalog.remove_directory(directory.id),
               storage::StorageErrorCode::ReferencedDirectory);
  EXPECT_FALSE(catalog.unpersisted());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.remove_entry(memo.id)));
  EXPECT_TRUE(catalog.unpersisted());
  catalog.mark_persisted();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.remove_directory(directory.id)));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_FALSE(catalog.find_entry(memo.id).has_value());
  EXPECT_FALSE(catalog.find_directory(directory.id).has_value());
}

TEST(MetadataCatalogTest, RejectsDuplicatesInvalidValuesAndUnsafeLocators) {
  storage::MetadataCatalog catalog;
  const auto memo = memo_snapshot();
  const auto first_path = fs::path{"memos/first.md"};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, first_path))));
  catalog.mark_persisted();
  expect_error(catalog.insert_entry(storage::metadata_from(memo, "memos/second.md")),
               storage::StorageErrorCode::DuplicateId);
  EXPECT_FALSE(catalog.unpersisted());

  auto other = memo;
  other.id = id("97183823-2574-4bfd-b411-99ed177d3e43");
  expect_any_error(catalog.insert_entry(storage::metadata_from(other, first_path)));
  for (const auto& unsafe : {fs::path{"../outside.md"},
                             fs::path{"memos/../memos/entry.md"},
                             fs::path{"/absolute/entry.md"}}) {
    SCOPED_TRACE(unsafe.string());
    expect_any_error(catalog.insert_entry(storage::metadata_from(other, unsafe)));
  }
  auto bad_privacy = other;
  bad_privacy.privacy = static_cast<core::Privacy>(99);
  expect_any_error(catalog.insert_entry(storage::metadata_from(
      bad_privacy, "memos/other.md")));
  auto bad_utf8 = other;
  bad_utf8.title = std::string{static_cast<char>(0xff)};
  expect_any_error(catalog.insert_entry(storage::metadata_from(
      bad_utf8, "memos/other.md")));
  other.directory_id = id("a7183823-2574-4bfd-b411-99ed177d3e43");
  expect_any_error(catalog.insert_entry(storage::metadata_from(
      other, "memos/other.md")));
  auto valid_other = memo_snapshot();
  valid_other.id = other.id;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(valid_other, "memos/other.md"))));
  catalog.mark_persisted();
  expect_error(catalog.update_entry(storage::metadata_from(memo, "memos/other.md")),
               storage::StorageErrorCode::InvalidMetadata);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).relative_path,
            first_path);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(valid_other.id)).relative_path,
            fs::path{"memos/other.md"});
  EXPECT_FALSE(catalog.unpersisted());
  ASSERT_TRUE(catalog.find_entry(memo.id).has_value());
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).relative_path,
            first_path);
  EXPECT_TRUE(catalog.find_entry(other.id).has_value());

  auto bad_todo = todo_snapshot();
  bad_todo.status = static_cast<core::TodoStatus>(99);
  expect_error(catalog.insert_entry(storage::metadata_from(bad_todo,
      "todos/invalid.toml")), storage::StorageErrorCode::InvalidMetadata);
  bad_todo = todo_snapshot();
  bad_todo.priority = static_cast<core::TodoPriority>(99);
  expect_error(catalog.insert_entry(storage::metadata_from(bad_todo,
      "todos/invalid.toml")), storage::StorageErrorCode::InvalidMetadata);

  const core::Directory::Snapshot directory{
      id("b7183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "Group"};
  const auto folder = fs::path{"memos"} / directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(directory, folder))));
  catalog.mark_persisted();
  auto other_kind = directory;
  other_kind.kind = storage::DirectoryKind::Todo;
  expect_error(catalog.insert_directory(storage::metadata_from(
      other_kind, fs::path{"todos"} / directory.id.to_string())),
      storage::StorageErrorCode::DuplicateId);
  EXPECT_FALSE(catalog.unpersisted());
  auto unsafe_folder = storage::metadata_from(
      directory, "memos/../other-name");
  expect_any_error(catalog.update_directory(unsafe_folder));
  EXPECT_EQ(catalog.find_directory(directory.id)->relative_path, folder);
  EXPECT_FALSE(catalog.unpersisted());
}

TEST(MetadataCatalogTest, ListsModuleRootsAndDirectoryChildrenNonrecursively) {
  storage::MetadataCatalog catalog;
  const core::Directory::Snapshot memo_directory{
      id("87183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "Memo group"};
  const core::Directory::Snapshot todo_directory{
      id("97183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Todo,
      "Todo group"};
  const auto memo_folder = fs::path{"memos"} / memo_directory.id.to_string();
  const auto todo_folder = fs::path{"todos"} / todo_directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(memo_directory, memo_folder))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(todo_directory, todo_folder))));
  const auto log = log_snapshot();
  const auto root_memo = memo_snapshot();
  auto grouped_memo = memo_snapshot(memo_directory.id);
  grouped_memo.id = id("a7183823-2574-4bfd-b411-99ed177d3e43");
  const auto grouped_todo = todo_snapshot(todo_directory.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(log, "logs/log.md"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(root_memo, "memos/root.md"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(grouped_memo, memo_folder / "child.md"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(grouped_todo, todo_folder / "task.toml"))));

  const auto log_root = catalog.list_root(storage::EntryType::Log);
  ASSERT_EQ(log_root.entries.size(), 1U);
  EXPECT_TRUE(log_root.directories.empty());
  const auto memo_root = catalog.list_root(storage::EntryType::Memo);
  ASSERT_EQ(memo_root.entries.size(), 1U);
  ASSERT_EQ(memo_root.directories.size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(memo_root.entries.front()).id,
            root_memo.id);
  EXPECT_EQ(memo_root.directories.front().id, memo_directory.id);
  const auto todo_root = catalog.list_root(storage::EntryType::Todo);
  EXPECT_TRUE(todo_root.entries.empty());
  ASSERT_EQ(todo_root.directories.size(), 1U);
  EXPECT_EQ(todo_root.directories.front().id, todo_directory.id);
  const auto children = catalog.list_directory(storage::DirectoryKind::Memo,
                                                memo_directory.id);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(children));
  ASSERT_EQ(std::get<std::vector<storage::EntryMetadata>>(children).size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(
                std::get<std::vector<storage::EntryMetadata>>(children).front()).id,
            grouped_memo.id);
  EXPECT_EQ(catalog.list_directories(storage::DirectoryKind::Memo).size(), 1U);
  EXPECT_EQ(catalog.list_directories(storage::DirectoryKind::Todo).size(), 1U);
  expect_any_error(catalog.list_directory(storage::DirectoryKind::Todo,
                                          memo_directory.id));
  EXPECT_TRUE(catalog.unpersisted());
}

TEST(MetadataCatalogTest, QueriesAcrossModulesWithIndependentFilters) {
  storage::MetadataCatalog catalog;
  const core::Directory::Snapshot memo_directory{
      id("87183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "旅行"};
  const core::Directory::Snapshot todo_directory{
      id("97183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Todo,
      "Tasks"};
  const auto memo_folder = fs::path{"memo-vault"} / memo_directory.id.to_string();
  const auto todo_folder = fs::path{"todo-vault"} / todo_directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(memo_directory, memo_folder))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(todo_directory, todo_folder))));
  auto log = log_snapshot();
  auto log_two = log;
  log_two.id = id("07183823-2574-4bfd-b411-99ed177d3e43");
  log_two.title = "Gamma";
  log_two.log_date.date = std::chrono::year{2028} / 1 / 1;
  log_two.tags = {{"other"}};
  auto memo = memo_snapshot(memo_directory.id);
  auto root_memo = memo_snapshot();
  root_memo.id = id("17183823-2574-4bfd-b411-99ed177d3e43");
  root_memo.title = "ALPHA root";
  auto todo = todo_snapshot(todo_directory.id);
  auto no_due = todo_snapshot();
  no_due.id = id("27183823-2574-4bfd-b411-99ed177d3e43");
  no_due.title = "No due";
  no_due.due_date.reset();
  no_due.status = core::TodoStatus::Pending;
  no_due.priority = core::TodoPriority::Low;
  auto date_due = todo_snapshot();
  date_due.id = id("37183823-2574-4bfd-b411-99ed177d3e43");
  date_due.title = "Date due";
  date_due.due_date = core::DateContext{
      {std::chrono::year{2027} / 3 / 3}, "UTC", std::nullopt};
  for (const auto& entry : {
           storage::metadata_from(log, "log-vault/one.enc"),
           storage::metadata_from(log_two, "log-vault/two.enc"),
           storage::metadata_from(memo, memo_folder / "one.enc"),
           storage::metadata_from(root_memo, "memo-vault/root.enc"),
           storage::metadata_from(todo, todo_folder / "one.enc"),
           storage::metadata_from(no_due, "todo-vault/root.enc"),
           storage::metadata_from(date_due, "todo-vault/date.enc")}) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(entry)));
  }
  catalog.mark_persisted();
  storage::CatalogQuery query;
  EXPECT_EQ(catalog.query(query).size(), 7U);
  query.type = storage::EntryType::Memo;
  EXPECT_EQ(catalog.query(query).size(), 2U);
  query.type.reset();
  query.directory = std::optional<core::Id>{};
  EXPECT_EQ(catalog.query(query).size(), 5U);
  query.directory = std::optional<core::Id>{memo_directory.id};
  ASSERT_EQ(catalog.query(query).size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(catalog.query(query).front()).id, memo.id);
  query.directory.reset();
  query.privacy = core::Privacy::Hidden;
  EXPECT_EQ(catalog.query(query).size(), 3U);
  query.privacy.reset();
  query.text = "alpha";
  EXPECT_EQ(catalog.query(query).size(), 2U);
  query.text = "旅行";
  ASSERT_EQ(catalog.query(query).size(), 1U);
  EXPECT_EQ(std::get<storage::LogMetadata>(catalog.query(query).front()).id, log.id);
  query.text = "CAFÉ";
  EXPECT_TRUE(catalog.query(query).empty());
  query.text = "Café";
  ASSERT_EQ(catalog.query(query).size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(catalog.query(query).front()).id, memo.id);
  query.text = "Private notes";
  EXPECT_TRUE(catalog.query(query).empty());
  query.text = "";
  query.tag = core::Tag{"TRAV"};
  ASSERT_EQ(catalog.query(query).size(), 1U);
  EXPECT_EQ(std::get<storage::LogMetadata>(catalog.query(query).front()).id, log.id);
  query.tag = core::Tag{"旅行"};
  EXPECT_EQ(catalog.query(query).size(), 3U);
  query.tag.reset();
  query.created.from = log.created.timestamp;
  query.created.through = log.created.timestamp;
  EXPECT_EQ(catalog.query(query).size(), 2U);
  query.created = {};
  query.modified.from = todo.modified.timestamp;
  EXPECT_EQ(catalog.query(query).size(), 3U);
  query.modified = {};
  query.log_date.from = std::chrono::year{2027} / 1 / 15;
  query.log_date.through = std::chrono::year{2027} / 1 / 15;
  ASSERT_EQ(catalog.query(query).size(), 1U);
  EXPECT_EQ(std::get<storage::LogMetadata>(catalog.query(query).front()).id, log.id);
  query.log_date = {};
  query.todo_status = core::TodoStatus::Completed;
  query.todo_priority = core::TodoPriority::High;
  EXPECT_EQ(catalog.query(query).size(), 2U);
  query.todo_status.reset();
  query.todo_priority.reset();
  query.has_due_date = false;
  EXPECT_EQ(catalog.query(query).size(), 1U);
  query.has_due_date = true;
  EXPECT_EQ(catalog.query(query).size(), 2U);
  query.has_due_date.reset();
  query.due_kind = storage::DueKind::Date;
  EXPECT_EQ(catalog.query(query).size(), 1U);
  query.due_kind = storage::DueKind::Datetime;
  EXPECT_EQ(catalog.query(query).size(), 1U);
  query.due_kind.reset();
  query.due_from = core::DueDate{core::DateContext{
      {std::chrono::year{2027} / 3 / 3}, "UTC", std::nullopt}};
  query.due_through = query.due_from;
  EXPECT_EQ(catalog.query(query).size(), 1U);
  query.due_from.reset();
  query.due_through.reset();
  EXPECT_FALSE(catalog.unpersisted());
}

TEST(MetadataCatalogTest, SortingIsStableByUuidAndKeepsMissingKeysLast) {
  storage::MetadataCatalog catalog;
  auto first = log_snapshot();
  first.id = id("17183823-2574-4bfd-b411-99ed177d3e43");
  first.title = "Same";
  auto second = first;
  second.id = id("07183823-2574-4bfd-b411-99ed177d3e43");
  auto memo = memo_snapshot();
  memo.title = "Earlier";
  auto todo = todo_snapshot();
  for (const auto& entry : {
           storage::metadata_from(first, "logs/a.md"),
           storage::metadata_from(second, "logs/b.md"),
           storage::metadata_from(memo, "memos/a.md"),
           storage::metadata_from(todo, "todos/a.toml")}) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(entry)));
  }
  storage::CatalogQuery query;
  query.sort = storage::MetadataSort::Title;
  auto sorted = catalog.query(query);
  ASSERT_EQ(sorted.size(), 4U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(sorted[0]).id, memo.id);
  EXPECT_EQ(std::get<storage::LogMetadata>(sorted[1]).id, second.id);
  EXPECT_EQ(std::get<storage::LogMetadata>(sorted[2]).id, first.id);
  query.descending = true;
  sorted = catalog.query(query);
  ASSERT_EQ(sorted.size(), 4U);
  EXPECT_EQ(std::get<storage::LogMetadata>(sorted[1]).id, second.id);
  EXPECT_EQ(std::get<storage::LogMetadata>(sorted[2]).id, first.id);
  query.sort = storage::MetadataSort::LogDate;
  sorted = catalog.query(query);
  ASSERT_EQ(sorted.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<storage::LogMetadata>(sorted[0]));
  EXPECT_TRUE(std::holds_alternative<storage::LogMetadata>(sorted[1]));
  query.sort = storage::MetadataSort::DueDate;
  query.descending = false;
  sorted = catalog.query(query);
  ASSERT_EQ(sorted.size(), 4U);
  EXPECT_EQ(std::get<storage::TodoMetadata>(sorted[0]).id, todo.id);
  query.descending = true;
  sorted = catalog.query(query);
  ASSERT_EQ(sorted.size(), 4U);
  EXPECT_EQ(std::get<storage::TodoMetadata>(sorted[0]).id, todo.id);
}

TEST(MetadataCatalogTest, EmptyTitleMatchesUnfilteredQuery) {
  storage::MetadataCatalog catalog;
  auto memo = memo_snapshot();
  memo.title.clear();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, "memos/untitled.md"))));
  catalog.mark_persisted();
  const auto all = catalog.query();
  ASSERT_EQ(all.size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(all.front()).id, memo.id);
  EXPECT_FALSE(catalog.unpersisted());
}

TEST(MetadataCatalogTest, MovesAndRefreshesMetadataWithoutChangingIdentity) {
  storage::MetadataCatalog catalog;
  const core::Directory::Snapshot memo_dir{
      id("87183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Memo,
      "Memo"};
  const core::Directory::Snapshot todo_dir{
      id("97183823-2574-4bfd-b411-99ed177d3e43"), storage::DirectoryKind::Todo,
      "Todo"};
  const auto memo_folder = fs::path{"memos"} / memo_dir.id.to_string();
  const auto todo_folder = fs::path{"todos"} / todo_dir.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(memo_dir, memo_folder))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_directory(
      storage::metadata_from(todo_dir, todo_folder))));
  auto memo = memo_snapshot();
  auto todo = todo_snapshot(todo_dir.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, "memos/root.md"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(todo, todo_folder / "task.toml"))));
  catalog.mark_persisted();

  auto invalid_memo = memo;
  invalid_memo.directory_id = todo_dir.id;
  expect_error(catalog.update_entry(storage::metadata_from(
      invalid_memo, todo_folder / "bad.md")),
      storage::StorageErrorCode::InvalidMetadata);
  auto invalid_todo = todo;
  invalid_todo.directory_id = memo_dir.id;
  expect_error(catalog.update_entry(storage::metadata_from(
      invalid_todo, memo_folder / "bad.toml")),
      storage::StorageErrorCode::InvalidMetadata);
  EXPECT_FALSE(catalog.unpersisted());
  EXPECT_EQ(catalog.list_root(storage::EntryType::Memo).entries.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).entries.size(), 0U);

  memo.directory_id = memo_dir.id;
  memo.privacy = core::Privacy::Hidden;
  memo.created = time_at(std::chrono::seconds{1800000500}, "UTC", "新地点");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::metadata_from(memo, memo_folder / "moved.md"))));
  todo.directory_id.reset();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::metadata_from(todo, "todos/moved.toml"))));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_TRUE(catalog.list_root(storage::EntryType::Memo).entries.empty());
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).entries.size(), 1U);
  const auto memo_children = catalog.list_directory(storage::DirectoryKind::Memo,
                                                     memo_dir.id);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(memo_children));
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(memo_children).size(), 1U);
  const auto todo_children = catalog.list_directory(storage::DirectoryKind::Todo,
                                                     todo_dir.id);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(todo_children));
  EXPECT_TRUE(std::get<std::vector<storage::EntryMetadata>>(todo_children).empty());
  const auto saved = std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id));
  EXPECT_EQ(saved.privacy, core::Privacy::Hidden);
  expect_context(saved.created, memo.created);
  EXPECT_EQ(saved.relative_path, memo_folder / "moved.md");

  catalog.mark_persisted();
  auto wrong_type = storage::metadata_from(log_snapshot(), "logs/wrong.md");
  std::get<storage::LogMetadata>(wrong_type).id = memo.id;
  expect_error(catalog.update_entry(wrong_type), storage::StorageErrorCode::InvalidMetadata);
  expect_error(catalog.remove_entry(id("a7183823-2574-4bfd-b411-99ed177d3e43")),
               storage::StorageErrorCode::NotFound);
  expect_error(catalog.remove_directory(id("a7183823-2574-4bfd-b411-99ed177d3e43")),
               storage::StorageErrorCode::NotFound);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).relative_path,
            saved.relative_path);
  EXPECT_FALSE(catalog.unpersisted());
}

TEST(MetadataCatalogTest, ReverseMembershipFollowsMovesAndGuardsKindChanges) {
  storage::MetadataCatalog catalog;
  const core::Directory::Snapshot first{
      id("87183823-2574-4bfd-b411-99ed177d3e43"),
      storage::DirectoryKind::Memo, "First"};
  const core::Directory::Snapshot second{
      id("97183823-2574-4bfd-b411-99ed177d3e43"),
      storage::DirectoryKind::Memo, "Second"};
  const core::Directory::Snapshot tasks{
      id("a7183823-2574-4bfd-b411-99ed177d3e43"),
      storage::DirectoryKind::Todo, "Tasks"};
  const auto first_path = fs::path{"memos"} / first.id.to_string();
  const auto second_path = fs::path{"memos"} / second.id.to_string();
  const auto tasks_path = fs::path{"todos"} / tasks.id.to_string();
  for (const auto& directory : {
           storage::metadata_from(first, first_path),
           storage::metadata_from(second, second_path),
           storage::metadata_from(tasks, tasks_path)}) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(
        catalog.insert_directory(directory)));
  }
  auto memo = memo_snapshot(first.id);
  auto todo = todo_snapshot(tasks.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, first_path / "memo.md"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(todo, tasks_path / "todo.toml"))));
  catalog.mark_persisted();

  auto cross_kind = first;
  cross_kind.kind = storage::DirectoryKind::Todo;
  expect_error(catalog.update_directory(storage::metadata_from(
      cross_kind, first_path)), storage::StorageErrorCode::ReferencedDirectory);
  expect_error(catalog.remove_directory(first.id),
               storage::StorageErrorCode::ReferencedDirectory);
  expect_error(catalog.remove_directory(tasks.id),
               storage::StorageErrorCode::ReferencedDirectory);
  EXPECT_FALSE(catalog.unpersisted());

  memo.directory_id = second.id;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::metadata_from(memo, second_path / "memo.md"))));
  const auto old_members = catalog.list_directory(storage::DirectoryKind::Memo,
                                                   first.id);
  const auto new_members = catalog.list_directory(storage::DirectoryKind::Memo,
                                                   second.id);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(old_members));
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(new_members));
  EXPECT_TRUE(std::get<std::vector<storage::EntryMetadata>>(old_members).empty());
  ASSERT_EQ(std::get<std::vector<storage::EntryMetadata>>(new_members).size(), 1U);
  EXPECT_EQ(std::get<storage::MemoMetadata>(
                std::get<std::vector<storage::EntryMetadata>>(new_members).front()).id,
            memo.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.remove_directory(first.id)));
  expect_error(catalog.remove_directory(second.id),
               storage::StorageErrorCode::ReferencedDirectory);

  todo.directory_id.reset();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::metadata_from(todo, "todos/root.toml"))));
  EXPECT_TRUE(std::get<std::vector<storage::EntryMetadata>>(
      catalog.list_directory(storage::DirectoryKind::Todo, tasks.id)).empty());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.remove_directory(tasks.id)));

  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.remove_entry(memo.id)));
  auto retagged = second;
  retagged.kind = storage::DirectoryKind::Todo;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_directory(
      storage::metadata_from(retagged, second_path))));
  EXPECT_TRUE(catalog.list_directories(storage::DirectoryKind::Memo).empty());
  ASSERT_EQ(catalog.list_directories(storage::DirectoryKind::Todo).size(), 1U);
  EXPECT_EQ(catalog.list_directories(storage::DirectoryKind::Todo).front().id,
            second.id);
  EXPECT_TRUE(catalog.list_root(storage::EntryType::Memo).directories.empty());
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).directories.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).entries.size(), 1U);
}

TEST(MetadataCatalogTest, SortsTypedFieldsAndDueVariants) {
  storage::MetadataCatalog catalog;
  auto log = log_snapshot();
  auto memo = memo_snapshot();
  auto dated = todo_snapshot();
  dated.due_date = core::DateContext{
      {std::chrono::year{2027} / 2 / 3}, "UTC", std::nullopt};
  auto timed = todo_snapshot();
  timed.id = id("77183823-2574-4bfd-b411-99ed177d3e43");
  timed.status = core::TodoStatus::Pending;
  timed.priority = core::TodoPriority::Low;
  timed.modified = time_at(std::chrono::seconds{1800000400}, "UTC", std::nullopt);
  auto undated = todo_snapshot();
  undated.id = id("27183823-2574-4bfd-b411-99ed177d3e43");
  undated.due_date.reset();
  for (const auto& entry : {
           storage::metadata_from(log, "logs/a.md"),
           storage::metadata_from(memo, "memos/a.md"),
           storage::metadata_from(dated, "todos/date.toml"),
           storage::metadata_from(timed, "todos/time.toml"),
           storage::metadata_from(undated, "todos/none.toml")}) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(entry)));
  }
  storage::CatalogQuery query;
  query.sort = storage::MetadataSort::Type;
  auto ordered = catalog.query(query);
  ASSERT_EQ(ordered.size(), 5U);
  EXPECT_EQ(storage::entry_type(ordered[0]), storage::EntryType::Log);
  EXPECT_EQ(storage::entry_type(ordered[1]), storage::EntryType::Memo);
  query.sort = storage::MetadataSort::Created;
  ordered = catalog.query(query);
  EXPECT_EQ(std::get<storage::LogMetadata>(ordered[0]).id, log.id);
  EXPECT_EQ(std::get<storage::MemoMetadata>(ordered[1]).id, memo.id);
  query.sort = storage::MetadataSort::Modified;
  ordered = catalog.query(query);
  EXPECT_EQ(std::get<storage::LogMetadata>(ordered[0]).id, log.id);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered.back()).id, timed.id);
  query.sort = storage::MetadataSort::DueDate;
  ordered = catalog.query(query);
  ASSERT_EQ(ordered.size(), 5U);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[0]).id, dated.id);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[1]).id, timed.id);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[2]).id, undated.id);
  query.sort = storage::MetadataSort::Status;
  ordered = catalog.query(query);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[0]).id, timed.id);
  query.sort = storage::MetadataSort::Priority;
  ordered = catalog.query(query);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[0]).id, timed.id);
  query.due_from = timed.due_date;
  query.due_through = timed.due_date;
  query.sort = storage::MetadataSort::Id;
  ordered = catalog.query(query);
  ASSERT_EQ(ordered.size(), 1U);
  EXPECT_EQ(std::get<storage::TodoMetadata>(ordered[0]).id, timed.id);
}

TEST(MetadataCatalogTest, RejectsIdentityAndLocatorCollisionsAcrossEntryTypes) {
  storage::MetadataCatalog catalog;
  const auto log = log_snapshot();
  const auto memo = memo_snapshot();
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(log, "mixed/log.enc"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, "mixed/memo.enc"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(todo, "mixed/todo.enc"))));
  catalog.mark_persisted();

  auto duplicate_log = log;
  duplicate_log.id = memo.id;
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_log, "mixed/new-log.enc")), storage::StorageErrorCode::DuplicateId);
  auto duplicate_memo = memo;
  duplicate_memo.id = todo.id;
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_memo, "mixed/new-memo.enc")), storage::StorageErrorCode::DuplicateId);
  auto duplicate_todo = todo;
  duplicate_todo.id = log.id;
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_todo, "mixed/new-todo.enc")), storage::StorageErrorCode::DuplicateId);

  duplicate_log.id = id("07183823-2574-4bfd-b411-99ed177d3e43");
  duplicate_memo.id = id("17183823-2574-4bfd-b411-99ed177d3e43");
  duplicate_todo.id = id("27183823-2574-4bfd-b411-99ed177d3e43");
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_log, "mixed/memo.enc")), storage::StorageErrorCode::InvalidMetadata);
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_memo, "mixed/todo.enc")), storage::StorageErrorCode::InvalidMetadata);
  expect_error(catalog.insert_entry(storage::metadata_from(
      duplicate_todo, "mixed/log.enc")), storage::StorageErrorCode::InvalidMetadata);
  EXPECT_FALSE(catalog.unpersisted());
  EXPECT_EQ(storage::entry_type(*catalog.find_entry(log.id)), storage::EntryType::Log);
  EXPECT_EQ(storage::entry_type(*catalog.find_entry(memo.id)), storage::EntryType::Memo);
  EXPECT_EQ(storage::entry_type(*catalog.find_entry(todo.id)), storage::EntryType::Todo);
  EXPECT_EQ(catalog.query().size(), 3U);
}

TEST(MetadataCatalogTest, CrossTypeQueriesAndMutationsKeepOtherEntriesIntact) {
  storage::MetadataCatalog catalog;
  auto log = log_snapshot();
  auto memo = memo_snapshot();
  auto todo = todo_snapshot();
  log.title = "Zulu";
  memo.title = "Alpha";
  todo.title = "Mike";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(todo, "entries/todo.enc"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(log, "entries/log.enc"))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo, "entries/memo.enc"))));
  catalog.mark_persisted();

  storage::CatalogQuery query;
  query.sort = storage::MetadataSort::Title;
  const auto ordered = catalog.query(query);
  ASSERT_EQ(ordered.size(), 3U);
  EXPECT_EQ(storage::entry_type(ordered[0]), storage::EntryType::Memo);
  EXPECT_EQ(storage::entry_type(ordered[1]), storage::EntryType::Todo);
  EXPECT_EQ(storage::entry_type(ordered[2]), storage::EntryType::Log);
  for (const auto type : {storage::EntryType::Log, storage::EntryType::Memo,
                          storage::EntryType::Todo}) {
    query.type = type;
    const auto filtered = catalog.query(query);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(storage::entry_type(filtered.front()), type);
    const auto listing = catalog.list_root(type);
    ASSERT_EQ(listing.entries.size(), 1U);
    EXPECT_EQ(storage::entry_type(listing.entries.front()), type);
  }
  EXPECT_FALSE(catalog.unpersisted());

  log.title = "Updated Log";
  memo.title = "Updated Memo";
  todo.title = "Updated Todo";
  for (const auto& update : {
           storage::metadata_from(log, "entries/log.enc"),
           storage::metadata_from(memo, "entries/memo.enc"),
           storage::metadata_from(todo, "entries/todo.enc")}) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(update)));
    EXPECT_TRUE(catalog.unpersisted());
    catalog.mark_persisted();
  }
  EXPECT_EQ(std::get<storage::LogMetadata>(*catalog.find_entry(log.id)).title,
            "Updated Log");
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).title,
            "Updated Memo");
  EXPECT_EQ(std::get<storage::TodoMetadata>(*catalog.find_entry(todo.id)).title,
            "Updated Todo");

  expect_error(catalog.update_entry(storage::metadata_from(
      log, "entries/memo.enc")), storage::StorageErrorCode::InvalidMetadata);
  EXPECT_EQ(std::get<storage::LogMetadata>(*catalog.find_entry(log.id)).relative_path,
            fs::path{"entries/log.enc"});
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.find_entry(memo.id)).relative_path,
            fs::path{"entries/memo.enc"});
  auto wrong_type = storage::metadata_from(memo, "entries/memo.enc");
  std::get<storage::MemoMetadata>(wrong_type).id = log.id;
  expect_error(catalog.update_entry(wrong_type), storage::StorageErrorCode::InvalidMetadata);
  EXPECT_FALSE(catalog.unpersisted());
  const std::vector<core::Id> removal_order{memo.id, log.id, todo.id};
  for (std::size_t i = 0; i < removal_order.size(); ++i) {
    const auto& removed = removal_order[i];
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.remove_entry(removed)));
    EXPECT_TRUE(catalog.unpersisted());
    catalog.mark_persisted();
    EXPECT_FALSE(catalog.find_entry(removed).has_value());
    EXPECT_EQ(catalog.query().size(), removal_order.size() - i - 1);
    for (std::size_t remaining = i + 1; remaining < removal_order.size(); ++remaining) {
      EXPECT_TRUE(catalog.find_entry(removal_order[remaining]).has_value());
    }
  }
  EXPECT_TRUE(catalog.query().empty());
  expect_error(catalog.remove_entry(log.id), storage::StorageErrorCode::NotFound);
  EXPECT_FALSE(catalog.unpersisted());
}

}  // namespace
