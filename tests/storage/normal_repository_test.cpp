#include "storage/codec.hpp"
#include "storage/normal_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>

namespace {

namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = fs::temp_directory_path() /
          ("folio-repository-test-" + core::Id::generate().to_string());
      if (fs::create_directory(candidate)) {
        path_ = fs::canonical(candidate);
        return;
      }
    }
    throw std::runtime_error("Could not reserve a temporary test directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

class NormalRepositoryTest : public testing::Test {
 protected:
  TemporaryDirectory temporary;
  fs::path root = temporary.path() / "normal";
  storage::NormalRepository repository{root};
};

core::Id id(const char* text) {
  const auto parsed = core::Id::parse(text);
  if (!parsed) throw std::runtime_error("Invalid test ID");
  return *parsed;
}

core::TimestampContext created() {
  return {core::Timestamp{std::chrono::seconds{1800000000}},
          "Asia/Shanghai", std::string{"上海"}};
}

core::TimestampContext modified() {
  return {core::Timestamp{std::chrono::seconds{1800000060}},
          "Europe/Paris", std::string{"Paris"}};
}

core::Log::Snapshot log_snapshot() {
  return {id("47183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Journal 旅行", "first line\n+++\nlast line",
          {{std::chrono::year{2026} / 10 / 25}, "Asia/Shanghai", "上海"},
          {{"daily"}, {"旅行"}}, core::Privacy::Normal};
}

core::Memo::Snapshot memo_snapshot() {
  return {id("55183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Memo café", std::string{"body\0with nul", 13}, {{"research"}, {"筆記"}},
          std::nullopt, core::Privacy::Normal};
}

core::Todo::Snapshot todo_snapshot() {
  return {id("67183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Plan trip", "First line\n第二行 — café",
          core::TodoStatus::Pending, core::TodoPriority::High,
          core::DueDate{core::DateContext{{std::chrono::year{2027} / 1 / 2},
                                          "Asia/Tokyo", std::string{"Tokyo"}}},
          std::nullopt, core::Privacy::Normal,
          {{"Book train", true}, {"Reserve hotel", false}}};
}

std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not open test file");
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  ASSERT_FALSE(output.fail());
}

template <typename T>
void expect_error(const storage::Result<T>& result) {
  EXPECT_TRUE(std::holds_alternative<storage::StorageError>(result));
}

template <typename T>
void expect_error_code(const storage::Result<T>& result,
                       storage::StorageErrorCode code) {
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(result));
  EXPECT_EQ(std::get<storage::StorageError>(result).code, code);
}

template <typename T>
bool operation_succeeded(const storage::Result<T>& result) {
  if (std::holds_alternative<T>(result)) return true;
  const auto& error = std::get<storage::StorageError>(result);
  ADD_FAILURE() << "StorageError code=" << static_cast<int>(error.code)
                << " path=" << error.path << " message=" << error.message
                << " system_error=" << error.system_error.message();
  return false;
}

void expect_context(const core::TimestampContext& actual,
                    const core::TimestampContext& expected) {
  EXPECT_EQ(actual.timestamp, expected.timestamp);
  EXPECT_EQ(actual.timezone, expected.timezone);
  EXPECT_EQ(actual.location, expected.location);
}

void expect_log(const core::Log::Snapshot& actual,
                const core::Log::Snapshot& expected) {
  EXPECT_EQ(actual.id, expected.id);
  expect_context(actual.created, expected.created);
  expect_context(actual.modified, expected.modified);
  EXPECT_EQ(actual.title, expected.title);
  EXPECT_EQ(actual.content, expected.content);
  EXPECT_EQ(actual.log_date.date, expected.log_date.date);
  EXPECT_EQ(actual.log_date.timezone, expected.log_date.timezone);
  EXPECT_EQ(actual.log_date.location, expected.log_date.location);
  EXPECT_EQ(actual.tags, expected.tags);
  EXPECT_EQ(actual.privacy, expected.privacy);
}

void expect_memo(const core::Memo::Snapshot& actual,
                 const core::Memo::Snapshot& expected) {
  EXPECT_EQ(actual.id, expected.id);
  expect_context(actual.created, expected.created);
  expect_context(actual.modified, expected.modified);
  EXPECT_EQ(actual.title, expected.title);
  EXPECT_EQ(actual.content, expected.content);
  EXPECT_EQ(actual.tags, expected.tags);
  EXPECT_EQ(actual.directory_id, expected.directory_id);
  EXPECT_EQ(actual.privacy, expected.privacy);
}

void expect_todo(const core::Todo::Snapshot& actual,
                 const core::Todo::Snapshot& expected) {
  EXPECT_EQ(actual.id, expected.id);
  expect_context(actual.created, expected.created);
  expect_context(actual.modified, expected.modified);
  EXPECT_EQ(actual.title, expected.title);
  EXPECT_EQ(actual.notes, expected.notes);
  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(actual.priority, expected.priority);
  ASSERT_EQ(actual.due_date.has_value(), expected.due_date.has_value());
  if (actual.due_date) {
    ASSERT_EQ(actual.due_date->index(), expected.due_date->index());
    if (std::holds_alternative<core::DateContext>(*actual.due_date)) {
      const auto& actual_date = std::get<core::DateContext>(*actual.due_date);
      const auto& expected_date = std::get<core::DateContext>(*expected.due_date);
      EXPECT_EQ(actual_date.date, expected_date.date);
      EXPECT_EQ(actual_date.timezone, expected_date.timezone);
      EXPECT_EQ(actual_date.location, expected_date.location);
    } else {
      const auto& actual_time = std::get<core::TimestampContext>(*actual.due_date);
      const auto& expected_time = std::get<core::TimestampContext>(*expected.due_date);
      expect_context(actual_time, expected_time);
    }
  }
  EXPECT_EQ(actual.directory_id, expected.directory_id);
  EXPECT_EQ(actual.privacy, expected.privacy);
  ASSERT_EQ(actual.subtasks.size(), expected.subtasks.size());
  for (std::size_t index = 0; index < actual.subtasks.size(); ++index) {
    EXPECT_EQ(actual.subtasks[index].content, expected.subtasks[index].content);
    EXPECT_EQ(actual.subtasks[index].completed, expected.subtasks[index].completed);
  }
}

TEST_F(NormalRepositoryTest, CreatesLoadsUpdatesAndReloadsCompleteLogSnapshot) {
  auto original = log_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_log(original)));
  const auto first = repository.load_log(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Log::Snapshot>(first));
  expect_log(std::get<core::Log::Snapshot>(first), original);

  const auto before = repository.locate_entry(storage::EntryType::Log, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(before));
  EXPECT_EQ(std::get<fs::path>(before).parent_path(), fs::path{"logs"});
  const auto canonical_name = storage::normal_entry_filename(
      storage::EntryType::Log, original.id, original.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(canonical_name));
  EXPECT_EQ(std::get<fs::path>(before).filename(),
            std::get<std::string>(canonical_name));
  original.modified = modified();
  original.title = "Edited journal";
  original.content = "updated\r\nbody";
  original.log_date = {{std::chrono::year{2026} / 11 / 1}, "UTC", std::nullopt};
  original.tags = {{"edited"}};
  ASSERT_TRUE(operation_succeeded(repository.update_log(original)));
  const auto second = repository.load_log(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Log::Snapshot>(second));
  expect_log(std::get<core::Log::Snapshot>(second), original);
  const auto after = repository.locate_entry(storage::EntryType::Log, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(after));
  EXPECT_EQ(std::get<fs::path>(after), std::get<fs::path>(before));
}

TEST_F(NormalRepositoryTest, CreatesLoadsUpdatesAndReloadsCompleteMemoSnapshot) {
  auto original = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(original)));
  const auto first = repository.load_memo(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(first));
  expect_memo(std::get<core::Memo::Snapshot>(first), original);

  const auto before = repository.locate_entry(storage::EntryType::Memo, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(before));
  EXPECT_EQ(std::get<fs::path>(before).parent_path(), fs::path{"memos"});
  const auto canonical_name = storage::normal_entry_filename(
      storage::EntryType::Memo, original.id, original.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(canonical_name));
  EXPECT_EQ(std::get<fs::path>(before).filename(),
            std::get<std::string>(canonical_name));
  original.modified = modified();
  original.title = "Edited memo";
  original.content = "new content";
  original.tags = {{"updated"}};
  ASSERT_TRUE(operation_succeeded(repository.update_memo(original)));
  const auto second = repository.load_memo(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(second));
  expect_memo(std::get<core::Memo::Snapshot>(second), original);
  const auto after = repository.locate_entry(storage::EntryType::Memo, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(after));
  EXPECT_EQ(std::get<fs::path>(after), std::get<fs::path>(before));
}

TEST_F(NormalRepositoryTest, CreatesLoadsUpdatesAndReloadsCompleteTodoSnapshot) {
  auto original = todo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_todo(original)));
  const auto first = repository.load_todo(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(first));
  expect_todo(std::get<core::Todo::Snapshot>(first), original);

  const auto before = repository.locate_entry(storage::EntryType::Todo, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(before));
  EXPECT_EQ(std::get<fs::path>(before).parent_path(), fs::path{"todos"});
  const auto canonical_name = storage::normal_entry_filename(
      storage::EntryType::Todo, original.id, original.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(canonical_name));
  EXPECT_EQ(std::get<fs::path>(before).filename(),
            std::get<std::string>(canonical_name));
  original.modified = modified();
  original.title = "Complete trip plan";
  original.notes = "Updated notes\n旅行";
  original.status = core::TodoStatus::Completed;
  original.priority = core::TodoPriority::Medium;
  original.due_date = core::TimestampContext{
      core::Timestamp{std::chrono::seconds{1900000000}}, "UTC", std::nullopt};
  original.subtasks.push_back({"Pack", false});
  ASSERT_TRUE(operation_succeeded(repository.update_todo(original)));
  const auto second = repository.load_todo(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(second));
  expect_todo(std::get<core::Todo::Snapshot>(second), original);
  const auto after = repository.locate_entry(storage::EntryType::Todo, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(after));
  EXPECT_EQ(std::get<fs::path>(after), std::get<fs::path>(before));

  original.title.clear();
  original.notes.clear();
  ASSERT_TRUE(operation_succeeded(repository.update_todo(original)));
  const auto emptied = repository.load_todo(original.id);
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(emptied));
  expect_todo(std::get<core::Todo::Snapshot>(emptied), original);
}

TEST_F(NormalRepositoryTest, SuppliedLogPathBypassesUnrelatedMalformedEntry) {
  auto log = log_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_log(log)));
  const auto locator = repository.locate_entry(storage::EntryType::Log, log.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto path = root / std::get<fs::path>(locator);
  const auto malformed_path = root / "logs" / "unrelated.md";
  ASSERT_NO_FATAL_FAILURE(write_bytes(malformed_path, "not a Markdown entry"));

  expect_error(repository.load_log(log.id));
  expect_error(repository.update_log(log));
  const auto loaded = repository.load_log(log.id, std::get<fs::path>(locator));
  ASSERT_TRUE(std::holds_alternative<core::Log::Snapshot>(loaded));
  expect_log(std::get<core::Log::Snapshot>(loaded), log);
  expect_error(repository.load_log(log.id, fs::path{"logs/unrelated.md"}));

  const auto original_bytes = read_bytes(path);
  auto changed_identity = log;
  changed_identity.created = modified();
  expect_error(repository.update_log(changed_identity, std::get<fs::path>(locator)));
  EXPECT_EQ(read_bytes(path), original_bytes);

  log.title = "Updated through exact path";
  log.modified = modified();
  ASSERT_TRUE(operation_succeeded(repository.update_log(
      log, std::get<fs::path>(locator))));
  const auto reloaded = repository.load_log(log.id, std::get<fs::path>(locator));
  ASSERT_TRUE(std::holds_alternative<core::Log::Snapshot>(reloaded));
  expect_log(std::get<core::Log::Snapshot>(reloaded), log);
  EXPECT_EQ(read_bytes(malformed_path), "not a Markdown entry");
  EXPECT_TRUE(fs::is_regular_file(path));
}

TEST_F(NormalRepositoryTest, SuppliedMemoPathRejectsStaleAndUnsafeLocators) {
  auto first = memo_snapshot();
  auto second = memo_snapshot();
  second.id = id("a7183823-2574-4bfd-b411-99ed177d3e43");
  second.title = "Other memo";
  ASSERT_TRUE(operation_succeeded(repository.create_memo(first)));
  ASSERT_TRUE(operation_succeeded(repository.create_memo(second)));
  const auto first_locator = repository.locate_entry(storage::EntryType::Memo, first.id);
  const auto second_locator = repository.locate_entry(storage::EntryType::Memo, second.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(first_locator));
  ASSERT_TRUE(std::holds_alternative<fs::path>(second_locator));
  const auto first_path = root / std::get<fs::path>(first_locator);
  const auto second_path = root / std::get<fs::path>(second_locator);
  const auto first_bytes = read_bytes(first_path);
  const auto second_bytes = read_bytes(second_path);

  expect_error_code(repository.load_memo(first.id, std::get<fs::path>(second_locator)),
                    storage::StorageErrorCode::StaleLocator);
  expect_error_code(repository.load_memo(first.id, fs::path{"memos/missing.md"}),
                    storage::StorageErrorCode::StaleLocator);
  first.title = "Must not overwrite another entry";
  expect_error_code(repository.update_memo(
      first, std::get<fs::path>(second_locator)),
      storage::StorageErrorCode::StaleLocator);
  expect_error_code(repository.update_memo(first, fs::path{"memos/missing.md"}),
                    storage::StorageErrorCode::StaleLocator);
  EXPECT_EQ(read_bytes(first_path), first_bytes);
  EXPECT_EQ(read_bytes(second_path), second_bytes);

  for (const auto& unsafe : {fs::path{"../outside.md"},
                             fs::path{"memos/../memos/missing.md"},
                             fs::path{"logs/other.md"}, second_path}) {
    SCOPED_TRACE(unsafe.string());
    expect_error_code(repository.load_memo(first.id, unsafe),
                      storage::StorageErrorCode::InvalidPath);
    expect_error_code(repository.update_memo(first, unsafe),
                      storage::StorageErrorCode::InvalidPath);
  }

  const auto outside = temporary.path() / "outside.md";
  ASSERT_NO_FATAL_FAILURE(write_bytes(outside, "outside original"));
  const auto link = root / "memos" / "link.md";
  std::error_code ec;
  fs::create_symlink(outside, link, ec);
  if (ec) GTEST_SKIP() << "Cannot create a symbolic link: " << ec.message();
  expect_error_code(repository.load_memo(first.id, fs::path{"memos/link.md"}),
                    storage::StorageErrorCode::InvalidPath);
  expect_error_code(repository.update_memo(first, fs::path{"memos/link.md"}),
                    storage::StorageErrorCode::InvalidPath);
  EXPECT_EQ(read_bytes(outside), "outside original");
  EXPECT_EQ(read_bytes(first_path), first_bytes);
  EXPECT_EQ(read_bytes(second_path), second_bytes);
}

TEST_F(NormalRepositoryTest, SuppliedMemoPathValidatesDirectoryMetadata) {
  const core::Directory::Snapshot directory{
      id("e7183823-2574-4bfd-b411-99ed177d3e43"), "Research"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, directory)));
  auto memo = memo_snapshot();
  memo.directory_id = directory.id;
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto relative = std::get<fs::path>(locator);
  EXPECT_EQ(relative.parent_path(),
            fs::path{"memos"} / directory.id.to_string());
  const auto path = root / relative;
  const auto metadata = path.parent_path() / ".folio-dir.toml";
  const auto first = repository.load_memo(memo.id, relative);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(first));
  expect_memo(std::get<core::Memo::Snapshot>(first), memo);

  memo.title = "Updated within Directory";
  memo.modified = modified();
  ASSERT_TRUE(operation_succeeded(repository.update_memo(memo, relative)));
  const auto updated = repository.load_memo(memo.id, relative);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(updated));
  expect_memo(std::get<core::Memo::Snapshot>(updated), memo);
  const auto bytes_before_metadata_failure = read_bytes(path);

  ASSERT_TRUE(fs::remove(metadata));
  const auto failed_load = repository.load_memo(memo.id, relative);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed_load));
  EXPECT_NE(std::get<storage::StorageError>(failed_load).code,
            storage::StorageErrorCode::StaleLocator);
  memo.title = "Must not persist without Directory metadata";
  const auto failed_update = repository.update_memo(memo, relative);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed_update));
  EXPECT_NE(std::get<storage::StorageError>(failed_update).code,
            storage::StorageErrorCode::StaleLocator);
  EXPECT_EQ(read_bytes(path), bytes_before_metadata_failure);
}

TEST_F(NormalRepositoryTest, SuppliedTodoPathBypassesUnrelatedDuplicateId) {
  auto todo = todo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_todo(todo)));
  const auto locator = repository.locate_entry(storage::EntryType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto todo_path = root / std::get<fs::path>(locator);

  auto unrelated = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(unrelated)));
  const auto unrelated_locator = repository.locate_entry(storage::EntryType::Memo,
                                                          unrelated.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(unrelated_locator));
  const auto unrelated_path = root / std::get<fs::path>(unrelated_locator);
  const auto duplicate_path = root / "memos" / "duplicate.md";
  fs::copy_file(unrelated_path, duplicate_path);
  const auto unrelated_bytes = read_bytes(unrelated_path);

  expect_error_code(repository.load_todo(todo.id),
                    storage::StorageErrorCode::DuplicateId);
  expect_error_code(repository.update_todo(todo),
                    storage::StorageErrorCode::DuplicateId);
  const auto loaded = repository.load_todo(todo.id, std::get<fs::path>(locator));
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(loaded));
  expect_todo(std::get<core::Todo::Snapshot>(loaded), todo);

  todo.title = "Updated through exact Todo path";
  todo.notes = "Updated exact notes\nCafé";
  todo.modified = modified();
  ASSERT_TRUE(operation_succeeded(repository.update_todo(
      todo, std::get<fs::path>(locator))));
  const auto reloaded = repository.load_todo(todo.id, std::get<fs::path>(locator));
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(reloaded));
  expect_todo(std::get<core::Todo::Snapshot>(reloaded), todo);
  EXPECT_TRUE(fs::is_regular_file(todo_path));
  EXPECT_EQ(read_bytes(unrelated_path), unrelated_bytes);
  EXPECT_EQ(read_bytes(duplicate_path), unrelated_bytes);
}

TEST_F(NormalRepositoryTest, DirectoryMetadataPreservesUnicodeNameAndPhysicalUuid) {
  core::Directory::Snapshot directory{id("78183823-2574-4bfd-b411-99ed177d3e43"),
                                      "旅行 / 筆記"};
  ASSERT_TRUE(operation_succeeded(
      repository.create_directory(storage::DirectoryKind::Memo, directory)));
  const auto folder = root / "memos" / directory.id.to_string();
  EXPECT_TRUE(fs::is_directory(folder));
  EXPECT_TRUE(fs::is_regular_file(folder / ".folio-dir.toml"));
  for (const unsigned char byte : folder.filename().string()) EXPECT_LT(byte, 128);
  const auto loaded = repository.load_directory(storage::DirectoryKind::Memo, directory.id);
  ASSERT_TRUE(std::holds_alternative<core::Directory::Snapshot>(loaded));
  EXPECT_EQ(std::get<core::Directory::Snapshot>(loaded).id, directory.id);
  EXPECT_EQ(std::get<core::Directory::Snapshot>(loaded).name, directory.name);

  ASSERT_TRUE(operation_succeeded(repository.rename_directory(
      storage::DirectoryKind::Memo, directory.id, "新名稱 café")));
  const auto renamed = repository.load_directory(storage::DirectoryKind::Memo, directory.id);
  ASSERT_TRUE(std::holds_alternative<core::Directory::Snapshot>(renamed));
  EXPECT_EQ(std::get<core::Directory::Snapshot>(renamed).name, "新名稱 café");
  EXPECT_TRUE(fs::is_directory(folder));
}

TEST_F(NormalRepositoryTest, MovesMemoAndTodoBetweenRootAndDirectories) {
  const core::Directory::Snapshot memo_directory{
      id("88183823-2574-4bfd-b411-99ed177d3e43"), "Memo folder"};
  const core::Directory::Snapshot todo_directory{
      id("98183823-2574-4bfd-b411-99ed177d3e43"), "Todo folder"};
  const core::Directory::Snapshot second_memo_directory{
      id("a9183823-2574-4bfd-b411-99ed177d3e43"), "Second memo folder"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, memo_directory)));
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, second_memo_directory)));
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Todo, todo_directory)));
  auto memo = memo_snapshot();
  auto todo = todo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  ASSERT_TRUE(operation_succeeded(repository.create_todo(todo)));
  const auto memo_before = repository.locate_entry(storage::EntryType::Memo, memo.id);
  const auto todo_before = repository.locate_entry(storage::EntryType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_before));
  ASSERT_TRUE(std::holds_alternative<fs::path>(todo_before));

  ASSERT_TRUE(operation_succeeded(repository.move_memo(
      memo.id, memo_directory.id, modified())));
  ASSERT_TRUE(operation_succeeded(repository.move_todo(
      todo.id, todo_directory.id, modified())));
  const auto memo_in_folder = repository.locate_entry(storage::EntryType::Memo, memo.id);
  const auto todo_in_folder = repository.locate_entry(storage::EntryType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_in_folder));
  ASSERT_TRUE(std::holds_alternative<fs::path>(todo_in_folder));
  EXPECT_EQ(std::get<fs::path>(memo_in_folder).parent_path(),
            fs::path{"memos"} / memo_directory.id.to_string());
  EXPECT_EQ(std::get<fs::path>(todo_in_folder).parent_path(),
            fs::path{"todos"} / todo_directory.id.to_string());
  EXPECT_EQ(std::get<fs::path>(memo_in_folder).filename(),
            std::get<fs::path>(memo_before).filename());
  EXPECT_EQ(std::get<fs::path>(todo_in_folder).filename(),
            std::get<fs::path>(todo_before).filename());
  memo.directory_id = memo_directory.id;
  todo.directory_id = todo_directory.id;
  memo.modified = modified();
  todo.modified = modified();
  const auto loaded_memo = repository.load_memo(memo.id);
  const auto loaded_todo = repository.load_todo(todo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded_memo));
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(loaded_todo));
  expect_memo(std::get<core::Memo::Snapshot>(loaded_memo), memo);
  expect_todo(std::get<core::Todo::Snapshot>(loaded_todo), todo);

  ASSERT_TRUE(operation_succeeded(repository.move_memo(
      memo.id, second_memo_directory.id, modified())));
  const auto memo_in_second_folder = repository.locate_entry(storage::EntryType::Memo,
                                                              memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_in_second_folder));
  EXPECT_EQ(std::get<fs::path>(memo_in_second_folder).parent_path(),
            fs::path{"memos"} / second_memo_directory.id.to_string());
  EXPECT_EQ(std::get<fs::path>(memo_in_second_folder).filename(),
            std::get<fs::path>(memo_before).filename());
  memo.directory_id = second_memo_directory.id;
  const auto memo_after_second_move = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(memo_after_second_move));
  expect_memo(std::get<core::Memo::Snapshot>(memo_after_second_move), memo);

  ASSERT_TRUE(operation_succeeded(repository.move_memo(
      memo.id, std::nullopt, modified())));
  ASSERT_TRUE(operation_succeeded(repository.move_todo(
      todo.id, std::nullopt, modified())));
  const auto memo_after = repository.locate_entry(storage::EntryType::Memo, memo.id);
  const auto todo_after = repository.locate_entry(storage::EntryType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_after));
  ASSERT_TRUE(std::holds_alternative<fs::path>(todo_after));
  EXPECT_EQ(std::get<fs::path>(memo_after), std::get<fs::path>(memo_before));
  EXPECT_EQ(std::get<fs::path>(todo_after), std::get<fs::path>(todo_before));
}

TEST_F(NormalRepositoryTest, RejectsDuplicateIdsAndDuplicatePhysicalPaths) {
  auto memo = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  expect_error(repository.create_memo(memo));
  auto log = log_snapshot();
  log.id = memo.id;
  expect_error(repository.create_log(log));
  const auto loaded = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  expect_memo(std::get<core::Memo::Snapshot>(loaded), memo);
}

TEST_F(NormalRepositoryTest, RejectsDuplicateIdInAnotherPhysicalDirectory) {
  const core::Directory::Snapshot directory{
      id("b8183823-2574-4bfd-b411-99ed177d3e43"), "Research"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, directory)));
  const auto memo = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto original = root / std::get<fs::path>(locator);
  const auto duplicate = root / "memos" / directory.id.to_string() /
      original.filename();
  fs::copy_file(original, duplicate);
  expect_error(repository.locate_entry(storage::EntryType::Memo, memo.id));
  expect_error(repository.load_memo(memo.id));
  EXPECT_EQ(read_bytes(original), read_bytes(duplicate));
}

TEST_F(NormalRepositoryTest, UnrelatedDuplicateStoredIdBlocksOtherEntry) {
  auto original = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(original)));
  const auto original_locator = repository.locate_entry(storage::EntryType::Memo,
                                                         original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(original_locator));
  const auto original_path = root / std::get<fs::path>(original_locator);
  const auto original_bytes = read_bytes(original_path);

  auto duplicate = memo_snapshot();
  duplicate.id = id("b7183823-2574-4bfd-b411-99ed177d3e43");
  duplicate.title = "Unrelated duplicate";
  const auto encoded = core::storage::codec::serialize_markdown(duplicate);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  const auto first_name = storage::normal_entry_filename(
      storage::EntryType::Memo, duplicate.id, duplicate.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(first_name));
  const auto first_path = root / "memos" / std::get<std::string>(first_name);
  const auto second_path = root / "memos" / "unrelated-renamed.md";
  ASSERT_NO_FATAL_FAILURE(write_bytes(first_path, std::get<std::string>(encoded)));
  fs::copy_file(first_path, second_path);

  const auto located = repository.locate_entry(storage::EntryType::Memo, original.id);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(located));
  EXPECT_EQ(std::get<storage::StorageError>(located).code,
            storage::StorageErrorCode::DuplicateId);
  const auto loaded = repository.load_memo(original.id);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(loaded));
  EXPECT_EQ(std::get<storage::StorageError>(loaded).code,
            storage::StorageErrorCode::DuplicateId);
  original.title = "Must not update while another ID is duplicated";
  expect_error(repository.update_memo(original));
  auto third = memo_snapshot();
  third.id = id("c7183823-2574-4bfd-b411-99ed177d3e43");
  expect_error(repository.create_memo(third));
  EXPECT_EQ(read_bytes(original_path), original_bytes);
  EXPECT_EQ(read_bytes(first_path), read_bytes(second_path));
}

TEST_F(NormalRepositoryTest, StoredIdSurvivesRenamedFilenameUpdateAndMove) {
  const core::Directory::Snapshot directory{
      id("b9183823-2574-4bfd-b411-99ed177d3e43"), "Destination"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, directory)));
  auto stored = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(stored)));
  const auto original_locator = repository.locate_entry(storage::EntryType::Memo,
                                                         stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(original_locator));
  const auto original_path = root / std::get<fs::path>(original_locator);
  const auto original_bytes = read_bytes(original_path);

  const auto filename_id = id("c9183823-2574-4bfd-b411-99ed177d3e43");
  const auto other_name = storage::normal_entry_filename(
      storage::EntryType::Memo, filename_id, stored.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(other_name));
  const auto mismatched_path = original_path.parent_path() /
      std::get<std::string>(other_name);
  fs::rename(original_path, mismatched_path);
  ASSERT_FALSE(fs::exists(original_path));
  ASSERT_EQ(read_bytes(mismatched_path), original_bytes);

  const auto located = repository.locate_entry(storage::EntryType::Memo, stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(located));
  EXPECT_EQ(root / std::get<fs::path>(located), mismatched_path);
  const auto known_path = repository.locate_entry(
      storage::EntryType::Memo, std::get<fs::path>(located));
  ASSERT_TRUE(std::holds_alternative<fs::path>(known_path));
  EXPECT_EQ(std::get<fs::path>(known_path), std::get<fs::path>(located));
  const auto loaded = repository.load_memo(stored.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  expect_memo(std::get<core::Memo::Snapshot>(loaded), stored);
  expect_error(repository.locate_entry(storage::EntryType::Memo, filename_id));
  expect_error(repository.load_memo(filename_id));

  expect_error(repository.create_memo(stored));
  auto nominal = memo_snapshot();
  nominal.id = filename_id;
  expect_error(repository.create_memo(nominal));
  EXPECT_EQ(read_bytes(mismatched_path), original_bytes);

  stored.title = "Updated through stored ID";
  stored.modified = modified();
  ASSERT_TRUE(operation_succeeded(repository.update_memo(stored)));
  const auto after_update = repository.locate_entry(storage::EntryType::Memo, stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(after_update));
  EXPECT_EQ(root / std::get<fs::path>(after_update), mismatched_path);
  const auto updated = repository.load_memo(stored.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(updated));
  expect_memo(std::get<core::Memo::Snapshot>(updated), stored);

  ASSERT_TRUE(operation_succeeded(repository.move_memo(
      stored.id, directory.id, modified())));
  stored.directory_id = directory.id;
  const auto moved = repository.locate_entry(storage::EntryType::Memo, stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(moved));
  EXPECT_EQ(std::get<fs::path>(moved).filename(), mismatched_path.filename());
  EXPECT_EQ(std::get<fs::path>(moved).parent_path(),
            fs::path{"memos"} / directory.id.to_string());
  const auto moved_snapshot = repository.load_memo(stored.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(moved_snapshot));
  expect_memo(std::get<core::Memo::Snapshot>(moved_snapshot), stored);
  EXPECT_FALSE(fs::exists(original_path));
  EXPECT_FALSE(fs::exists(mismatched_path));

  const auto moved_path = root / std::get<fs::path>(moved);
  const auto arbitrary_path = moved_path.parent_path() / ".notes.tmp-draft.md";
  fs::rename(moved_path, arbitrary_path);
  const auto arbitrary_locator = repository.locate_entry(storage::EntryType::Memo,
                                                          stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(arbitrary_locator));
  EXPECT_EQ(root / std::get<fs::path>(arbitrary_locator), arbitrary_path);
  const auto known_arbitrary = repository.locate_entry(
      storage::EntryType::Memo, std::get<fs::path>(arbitrary_locator));
  ASSERT_TRUE(std::holds_alternative<fs::path>(known_arbitrary));
  EXPECT_EQ(std::get<fs::path>(known_arbitrary), std::get<fs::path>(arbitrary_locator));
  const auto arbitrary_loaded = repository.load_memo(stored.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(arbitrary_loaded));
  expect_memo(std::get<core::Memo::Snapshot>(arbitrary_loaded), stored);

  stored.title = "Updated after basename changed";
  ASSERT_TRUE(operation_succeeded(repository.update_memo(stored)));
  const auto after_rename_update = repository.locate_entry(
      storage::EntryType::Memo, stored.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(after_rename_update));
  EXPECT_EQ(root / std::get<fs::path>(after_rename_update), arbitrary_path);
  const auto updated_after_rename = repository.load_memo(stored.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(updated_after_rename));
  expect_memo(std::get<core::Memo::Snapshot>(updated_after_rename), stored);

  const auto duplicate_path = arbitrary_path.parent_path() / "duplicate.md";
  fs::copy_file(arbitrary_path, duplicate_path);
  expect_error(repository.locate_entry(storage::EntryType::Memo, stored.id));
  expect_error(repository.load_memo(stored.id));
  EXPECT_EQ(read_bytes(arbitrary_path), read_bytes(duplicate_path));
}

TEST_F(NormalRepositoryTest, OldIdCannotOverwriteDifferentEntryAtSamePath) {
  auto original = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(original)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, original.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto path = root / std::get<fs::path>(locator);

  auto injected = memo_snapshot();
  injected.id = id("c7183823-2574-4bfd-b411-99ed177d3e43");
  injected.title = "Different stored ID";
  const auto encoded = core::storage::codec::serialize_markdown(injected);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  const auto injected_bytes = std::get<std::string>(encoded);
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, injected_bytes));

  expect_error(repository.locate_entry(storage::EntryType::Memo, original.id));
  expect_error(repository.load_memo(original.id));
  const auto by_stored_id = repository.locate_entry(storage::EntryType::Memo, injected.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(by_stored_id));
  EXPECT_EQ(std::get<fs::path>(by_stored_id), std::get<fs::path>(locator));
  const auto by_known_path = repository.locate_entry(
      storage::EntryType::Memo, std::get<fs::path>(locator));
  ASSERT_TRUE(std::holds_alternative<fs::path>(by_known_path));
  EXPECT_EQ(std::get<fs::path>(by_known_path), std::get<fs::path>(locator));
  const auto loaded = repository.load_memo(injected.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  expect_memo(std::get<core::Memo::Snapshot>(loaded), injected);
  original.title = "Must not overwrite";
  original.modified = modified();
  expect_error(repository.update_memo(original));
  expect_error(repository.create_memo(injected));
  expect_error(repository.create_memo(original));
  EXPECT_EQ(read_bytes(path), injected_bytes);
}

TEST_F(NormalRepositoryTest, IgnoresNonformalFilesAndRejectsThemAsKnownPaths) {
  const auto memo = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto formal_path = root / std::get<fs::path>(locator);
  const auto formal_bytes = read_bytes(formal_path);
  const auto duplicate_text_path = root / "memos" / "copied-entry.txt";
  const auto malformed_text_path = root / "memos" / "malformed.txt";
  ASSERT_NO_FATAL_FAILURE(write_bytes(duplicate_text_path, formal_bytes));
  ASSERT_NO_FATAL_FAILURE(write_bytes(malformed_text_path, "not valid TOML or Markdown"));

  const auto still_located = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(still_located));
  EXPECT_EQ(std::get<fs::path>(still_located), std::get<fs::path>(locator));
  const auto loaded = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  expect_memo(std::get<core::Memo::Snapshot>(loaded), memo);
  expect_error(repository.locate_entry(
      storage::EntryType::Memo, fs::path{"memos/copied-entry.txt"}));
  expect_error(repository.locate_entry(
      storage::EntryType::Memo, fs::path{"memos/malformed.txt"}));

  auto other = memo_snapshot();
  other.id = id("d7183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(operation_succeeded(repository.create_memo(other)));
  EXPECT_EQ(read_bytes(formal_path), formal_bytes);
  EXPECT_EQ(read_bytes(duplicate_text_path), formal_bytes);
  EXPECT_EQ(read_bytes(malformed_text_path), "not valid TOML or Markdown");
}

TEST_F(NormalRepositoryTest, RejectsMissingOrInvalidDirectoryMetadataAndReferences) {
  const core::Directory::Snapshot directory{
      id("a8183823-2574-4bfd-b411-99ed177d3e43"), "Research"};
  auto memo = memo_snapshot();
  memo.directory_id = directory.id;
  expect_error(repository.create_memo(memo));
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, directory)));
  const auto metadata = root / "memos" / directory.id.to_string() / ".folio-dir.toml";
  const auto original_metadata = read_bytes(metadata);
  ASSERT_TRUE(fs::remove(metadata));
  expect_error(repository.load_directory(storage::DirectoryKind::Memo, directory.id));
  expect_error(repository.create_memo(memo));
  ASSERT_NO_FATAL_FAILURE(write_bytes(metadata, "schema_version = 1\nname = 'missing id'\n"));
  expect_error(repository.load_directory(storage::DirectoryKind::Memo, directory.id));
  expect_error(repository.create_memo(memo));
  auto wrong_id_metadata = original_metadata;
  const auto id_position = wrong_id_metadata.find(directory.id.to_string());
  ASSERT_NE(id_position, std::string::npos);
  wrong_id_metadata.replace(id_position, directory.id.to_string().size(),
                            "c8183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_NO_FATAL_FAILURE(write_bytes(metadata, wrong_id_metadata));
  expect_error(repository.load_directory(storage::DirectoryKind::Memo, directory.id));
  expect_error(repository.create_memo(memo));
  ASSERT_NO_FATAL_FAILURE(write_bytes(metadata, original_metadata));
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto loaded = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  EXPECT_EQ(std::get<core::Memo::Snapshot>(loaded).directory_id, directory.id);

  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto entry_path = root / std::get<fs::path>(locator);
  const auto entry_bytes = read_bytes(entry_path);
  ASSERT_NO_FATAL_FAILURE(write_bytes(metadata, wrong_id_metadata));
  expect_error(repository.load_directory(storage::DirectoryKind::Memo, directory.id));
  expect_error(repository.locate_entry(storage::EntryType::Memo, memo.id));
  expect_error(repository.load_memo(memo.id));
  expect_error(repository.rename_directory(storage::DirectoryKind::Memo,
                                           directory.id, "Must not rename"));
  auto other_memo = memo_snapshot();
  other_memo.id = id("f9183823-2574-4bfd-b411-99ed177d3e43");
  other_memo.directory_id.reset();
  expect_error(repository.create_memo(other_memo));
  EXPECT_EQ(read_bytes(entry_path), entry_bytes);
  EXPECT_EQ(read_bytes(metadata), wrong_id_metadata);
}

TEST_F(NormalRepositoryTest, RejectsDirectoryReferenceFromWrongEntryModule) {
  const core::Directory::Snapshot todo_directory{
      id("d8183823-2574-4bfd-b411-99ed177d3e43"), "Todo only"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Todo, todo_directory)));
  auto memo = memo_snapshot();
  memo.directory_id = todo_directory.id;
  expect_error(repository.create_memo(memo));
  EXPECT_FALSE(fs::exists(root / "memos" / todo_directory.id.to_string()));
}

TEST_F(NormalRepositoryTest, RejectsMalformedEntryWithoutReplacingItsBytes) {
  auto todo = todo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_todo(todo)));
  const auto locator = repository.locate_entry(storage::EntryType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto path = root / std::get<fs::path>(locator);
  const std::string malformed = "schema_version = 1\ninvalid = [";
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, malformed));
  expect_error(repository.load_todo(todo.id));
  todo.title = "Must not overwrite malformed source";
  expect_error(repository.update_todo(todo));
  EXPECT_EQ(read_bytes(path), malformed);
}

TEST_F(NormalRepositoryTest, RejectsTraversalAndSymbolicLinkEscape) {
  expect_error(repository.locate_entry(storage::EntryType::Memo,
                                       fs::path{"../outside.md"}));
  expect_error(repository.locate_entry(storage::EntryType::Memo,
                                       fs::path{"memos/../logs/entry.md"}));

  const auto outside = temporary.path() / "outside";
  ASSERT_TRUE(fs::create_directory(outside));
  ASSERT_TRUE(fs::create_directory(root));
  std::error_code ec;
  fs::create_directory_symlink(outside, root / "memos", ec);
  if (ec) GTEST_SKIP() << "Cannot create a directory symlink: " << ec.message();
  expect_error(repository.create_memo(memo_snapshot()));
  EXPECT_TRUE(fs::is_empty(outside));
}

TEST_F(NormalRepositoryTest, RejectsSymbolicLinkEntry) {
  auto memo = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto entry_path = root / std::get<fs::path>(locator);
  const auto outside_file = temporary.path() / "outside.md";
  ASSERT_NO_FATAL_FAILURE(write_bytes(outside_file, "outside original"));
  const auto saved_entry = read_bytes(entry_path);
  ASSERT_TRUE(fs::remove(entry_path));
  std::error_code ec;
  fs::create_symlink(outside_file, entry_path, ec);
  if (ec) GTEST_SKIP() << "Cannot create a file symlink: " << ec.message();
  expect_error(repository.load_memo(memo.id));
  memo.title = "Must not escape";
  expect_error(repository.update_memo(memo));
  EXPECT_EQ(read_bytes(outside_file), "outside original");
  EXPECT_EQ(read_bytes(entry_path), "outside original");
  EXPECT_NE(saved_entry, read_bytes(outside_file));
}

TEST_F(NormalRepositoryTest, RejectsSymbolicLinkChildDirectory) {
  const core::Directory::Snapshot directory{
      id("e8183823-2574-4bfd-b411-99ed177d3e43"), "Child"};
  ASSERT_TRUE(operation_succeeded(repository.create_directory(
      storage::DirectoryKind::Memo, directory)));
  const auto child = root / "memos" / directory.id.to_string();
  const auto outside_child = temporary.path() / "outside-child";
  fs::rename(child, outside_child);
  std::error_code ec;
  fs::create_directory_symlink(outside_child, child, ec);
  if (ec) GTEST_SKIP() << "Cannot create a directory symlink: " << ec.message();
  auto another_memo = memo_snapshot();
  another_memo.id = id("f8183823-2574-4bfd-b411-99ed177d3e43");
  another_memo.directory_id = directory.id;
  expect_error(repository.create_memo(another_memo));
  std::set<fs::path> outside_names;
  for (const auto& item : fs::directory_iterator(outside_child)) {
    outside_names.insert(item.path().filename());
  }
  EXPECT_EQ(outside_names, std::set<fs::path>{".folio-dir.toml"});
}

TEST_F(NormalRepositoryTest, FailedAtomicUpdatePreservesExistingBytesAndSnapshot) {
  auto memo = memo_snapshot();
  ASSERT_TRUE(operation_succeeded(repository.create_memo(memo)));
  const auto locator = repository.locate_entry(storage::EntryType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto path = root / std::get<fs::path>(locator);
  const auto original_bytes = read_bytes(path);
  const auto pending = path.parent_path() /
      ("." + path.filename().string() + ".tmp-" +
       core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, original_bytes));
  const auto while_pending = repository.locate_entry(storage::EntryType::Memo,
                                                      memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(while_pending));
  EXPECT_EQ(std::get<fs::path>(while_pending), std::get<fs::path>(locator));
  const auto loaded_while_pending = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded_while_pending));
  expect_memo(std::get<core::Memo::Snapshot>(loaded_while_pending), memo);
  memo.title = "Should not persist";
  memo.content = "new body";
  memo.modified = modified();
  expect_error(repository.update_memo(memo));
  EXPECT_EQ(read_bytes(path), original_bytes);
  EXPECT_EQ(read_bytes(pending), original_bytes);
  std::set<fs::path> siblings;
  for (const auto& item : fs::directory_iterator(path.parent_path())) {
    siblings.insert(item.path().filename());
  }
  EXPECT_EQ(siblings, (std::set<fs::path>{path.filename(), pending.filename()}));
  const auto loaded = repository.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  EXPECT_EQ(std::get<core::Memo::Snapshot>(loaded).title, "Memo café");
}

}  // namespace
