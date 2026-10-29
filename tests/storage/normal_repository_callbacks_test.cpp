#include "storage/catalog_coordinator.hpp"
#include "storage/normal_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;

core::Id id(const char* value) {
  const auto parsed = core::Id::parse(value);
  if (!parsed) throw std::runtime_error("Invalid fixture UUID");
  return *parsed;
}

core::TimestampContext created() {
  return {core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", std::nullopt};
}

core::TimestampContext modified() {
  return {core::Timestamp{std::chrono::seconds{1800000060}}, "Asia/Shanghai", "上海"};
}

core::Log::Snapshot log_snapshot() {
  return {id("17183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Log title", "first body",
          {{std::chrono::year{2027} / 1 / 15}, "UTC", std::nullopt},
          {{"daily"}}, core::Privacy::Normal};
}

core::Memo::Snapshot memo_snapshot() {
  return {id("27183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Memo title", "first body", {{"research"}}, std::nullopt,
          core::Privacy::Normal};
}

core::Todo::Snapshot todo_snapshot() {
  return {id("37183823-2574-4bfd-b411-99ed177d3e43"), created(), created(),
          "Todo title", "first notes", core::TodoStatus::Pending,
          core::TodoPriority::High,
          core::DueDate{core::DateContext{{std::chrono::year{2028} / 2 / 3},
                                          "Asia/Tokyo", "東京"}},
          std::nullopt, core::Privacy::Normal, {{"First subtask", false}}};
}

core::Directory::Snapshot memo_directory() {
  return {id("47183823-2574-4bfd-b411-99ed177d3e43"),
          core::DirectoryKind::Memo, "研究 / Café"};
}

core::Directory::Snapshot todo_directory() {
  return {id("57183823-2574-4bfd-b411-99ed177d3e43"),
          core::DirectoryKind::Todo, "任务"};
}

std::string bytes(const fs::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Could not read fixture file");
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& file, const std::string& value) {
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  output.close();
  ASSERT_FALSE(output.fail());
}

class NormalRepositoryCallbacksTest : public testing::Test {
 protected:
  struct TemporaryRoot {
    fs::path path;
    TemporaryRoot() {
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = fs::temp_directory_path() /
            ("folio-repository-callback-test-" + core::Id::generate().to_string());
        if (fs::create_directory(candidate)) {
          path = fs::canonical(candidate);
          return;
        }
      }
      throw std::runtime_error("Could not reserve test root");
    }
    ~TemporaryRoot() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } root;
  fs::path normal = root.path / "normal";
  fs::path vault = root.path / "vault";
  fs::path state = root.path / "state";
  fs::path cache = root.path / "cache";
  fs::path index = state / "index" / "normal.msgpack";
  storage::NormalRepository repository{normal};
  storage::MetadataCatalog owned_catalog;
  storage::CatalogCoordinator coordinator{
      storage::StoragePaths{normal, vault, state, cache}, owned_catalog};

  void SetUp() override {
    const auto loaded = coordinator.startup_load();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
    ASSERT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    const auto saved = coordinator.shutdown_save();
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
    ASSERT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
    ASSERT_FALSE(owned_catalog.unpersisted());
    ASSERT_TRUE(fs::is_regular_file(index));
  }

  static void expect_code(const ::folio::core::internal::Result<std::monostate,
                          storage::StorageError>& result,
                          storage::StorageErrorCode code) {
    ASSERT_TRUE(std::holds_alternative<storage::StorageError>(result));
    EXPECT_EQ(std::get<storage::StorageError>(result).code, code);
  }
};

TEST_F(NormalRepositoryCallbacksTest, CreatesAndUpdatesAllTypesWithoutIndexIo) {
  const auto prior_index = bytes(index);
  const auto memo_dir = memo_directory();
  const auto todo_dir = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, memo_dir, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Todo, todo_dir, coordinator.directory_creation_callback())));
  const auto log = log_snapshot();
  const auto memo = memo_snapshot();
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(todo, coordinator.todo_creation_callback())));
  const auto log_path = std::get<storage::LogMetadata>(
      *owned_catalog.find_entry(log.id)).relative_path;
  const auto memo_path = std::get<storage::MemoMetadata>(
      *owned_catalog.find_entry(memo.id)).relative_path;
  const auto todo_path = std::get<storage::TodoMetadata>(
      *owned_catalog.find_entry(todo.id)).relative_path;
  EXPECT_EQ(log_path.parent_path(), fs::path{"logs"});
  EXPECT_EQ(memo_path.parent_path(), fs::path{"memos"});
  EXPECT_EQ(todo_path.parent_path(), fs::path{"todos"});
  EXPECT_EQ(owned_catalog.query().size(), 3U);
  EXPECT_EQ(bytes(index), prior_index);

  // Explicit locators bypass unrelated malformed formal files in the same module.
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.md", "bad front matter"));
  auto edited_log = log;
  edited_log.title = "Edited Log";
  edited_log.content = "second body";
  edited_log.modified = modified();
  auto edited_memo = memo;
  edited_memo.title = "Edited Memo";
  edited_memo.content = "second body";
  edited_memo.modified = modified();
  auto edited_todo = todo;
  edited_todo.title = "Edited Todo";
  edited_todo.notes = "second notes";
  edited_todo.modified = modified();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_log(edited_log, log_path, coordinator.log_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_memo(edited_memo, memo_path, coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_todo(edited_todo, todo_path, coordinator.todo_update_callback())));
  EXPECT_EQ(std::get<storage::LogMetadata>(*owned_catalog.find_entry(log.id)).title,
            "Edited Log");
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.find_entry(memo.id)).title,
            "Edited Memo");
  EXPECT_EQ(std::get<storage::TodoMetadata>(*owned_catalog.find_entry(todo.id)).title,
            "Edited Todo");
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id, log_path)).content,
            "second body");
  EXPECT_EQ(std::get<core::Memo::Snapshot>(repository.load_memo(memo.id, memo_path)).content,
            "second body");
  EXPECT_EQ(std::get<core::Todo::Snapshot>(repository.load_todo(todo.id, todo_path)).notes,
            "second notes");
  EXPECT_EQ(bytes(index), prior_index);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_NE(bytes(index), prior_index);
  EXPECT_FALSE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest, StoredCallbackReferencesSurviveRefreshWithoutConstructorIo) {
  const auto other_normal = root.path / "other-normal";
  const auto other_vault = root.path / "other-vault";
  const auto other_state = root.path / "other-state";
  const auto other_cache = root.path / "other-cache";
  storage::MetadataCatalog other_catalog;
  storage::CatalogCoordinator other{
      storage::StoragePaths{other_normal, other_vault, other_state, other_cache}, other_catalog};
  EXPECT_FALSE(fs::exists(other_normal));
  EXPECT_FALSE(fs::exists(other_vault));
  EXPECT_FALSE(fs::exists(other_state));
  EXPECT_FALSE(fs::exists(other_cache));

  const auto* log_create = &other.log_creation_callback();
  const auto* log_update = &other.log_update_callback();
  const auto* memo_create = &other.memo_creation_callback();
  const auto* memo_update = &other.memo_update_callback();
  const auto* todo_create = &other.todo_creation_callback();
  const auto* todo_update = &other.todo_update_callback();
  const auto* directory_create = &other.directory_creation_callback();
  const auto* directory_update = &other.directory_update_callback();
  EXPECT_TRUE(static_cast<bool>(*log_create));
  EXPECT_TRUE(static_cast<bool>(*log_update));
  EXPECT_TRUE(static_cast<bool>(*memo_create));
  EXPECT_TRUE(static_cast<bool>(*memo_update));
  EXPECT_TRUE(static_cast<bool>(*todo_create));
  EXPECT_TRUE(static_cast<bool>(*todo_update));
  EXPECT_TRUE(static_cast<bool>(*directory_create));
  EXPECT_TRUE(static_cast<bool>(*directory_update));

  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(other.startup_load()));
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(other.refresh_all()));
  EXPECT_EQ(log_create, &other.log_creation_callback());
  EXPECT_EQ(log_update, &other.log_update_callback());
  EXPECT_EQ(memo_create, &other.memo_creation_callback());
  EXPECT_EQ(memo_update, &other.memo_update_callback());
  EXPECT_EQ(todo_create, &other.todo_creation_callback());
  EXPECT_EQ(todo_update, &other.todo_update_callback());
  EXPECT_EQ(directory_create, &other.directory_creation_callback());
  EXPECT_EQ(directory_update, &other.directory_update_callback());
  EXPECT_FALSE(fs::exists(other_state / "index" / "normal.msgpack"));
}

TEST_F(NormalRepositoryCallbacksTest, MovesAndDirectoryRenameKeepMembershipCurrent) {
  const auto memo_dir = memo_directory();
  auto memo_second = memo_dir;
  memo_second.id = id("67183823-2574-4bfd-b411-99ed177d3e43");
  memo_second.name = "Second";
  const auto todo_dir = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, memo_dir, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, memo_second, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Todo, todo_dir, coordinator.directory_creation_callback())));
  const auto memo = memo_snapshot();
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(todo, coordinator.todo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto persisted_index = bytes(index);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, memo_dir.id, modified(), coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_todo(todo.id, todo_dir.id, modified(), coordinator.todo_update_callback())));
  EXPECT_EQ(owned_catalog.list_root(storage::EntryType::Memo).entries.size(), 0U);
  EXPECT_EQ(owned_catalog.list_root(storage::EntryType::Todo).entries.size(), 0U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo, memo_dir.id)).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Todo, todo_dir.id)).size(), 1U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, memo_second.id, modified(), coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo, memo_dir.id)).size(), 0U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo, memo_second.id)).size(), 1U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.rename_directory(
      core::DirectoryKind::Memo, memo_second.id, "Renamed / 研究", coordinator.directory_update_callback())));
  EXPECT_EQ(owned_catalog.find_directory(memo_second.id)->name, "Renamed / 研究");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, std::nullopt, modified(), coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_todo(todo.id, std::nullopt, modified(), coordinator.todo_update_callback())));
  EXPECT_EQ(owned_catalog.list_root(storage::EntryType::Memo).entries.size(), 1U);
  EXPECT_EQ(owned_catalog.list_root(storage::EntryType::Todo).entries.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo, memo_second.id)).size(), 0U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Todo, todo_dir.id)).size(), 0U);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), persisted_index);
}

TEST_F(NormalRepositoryCallbacksTest, RepositoryFailureDoesNotInvokeCallback) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto prior_index = bytes(index);
  int callbacks = 0;
  storage::PersistedCallback<storage::LogMetadata> observer = [&](const storage::LogMetadata&) {
    ++callbacks;
    return ::folio::core::internal::Result<std::monostate, storage::StorageError>{std::monostate{}};
  };
  expect_code(repository.create_log(log, observer), storage::StorageErrorCode::DuplicateId);
  auto edited = log;
  edited.title = "Blocked edit";
  edited.modified = modified();
  const auto path = std::get<storage::LogMetadata>(
      *owned_catalog.find_entry(log.id)).relative_path;
  const auto pending = normal / path.parent_path() /
      ("." + path.filename().string() + ".tmp-" + core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished"));
  const auto failed = repository.update_log(edited, path, observer);
  expect_code(failed, storage::StorageErrorCode::RecoveryRequired);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed));
  EXPECT_FALSE(std::get<storage::StorageError>(failed).replacement_completed);
  EXPECT_EQ(callbacks, 0);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, StaleLocatorCanBeCorrectedByCallerDiscovery) {
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto prior_index = bytes(index);
  const auto old_path = std::get<storage::MemoMetadata>(
      *owned_catalog.find_entry(memo.id)).relative_path;
  const fs::path renamed = fs::path{"memos"} / "externally-renamed.md";
  fs::rename(normal / old_path, normal / renamed);
  auto edited = memo;
  edited.title = "Discovered edit";
  edited.modified = modified();
  const auto stale = repository.update_memo(edited, old_path, coordinator.memo_update_callback());
  expect_code(stale, storage::StorageErrorCode::StaleLocator);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.find_entry(memo.id))
                .relative_path, old_path);
  EXPECT_FALSE(owned_catalog.unpersisted());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_memo(edited, std::nullopt, coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.find_entry(memo.id))
                .relative_path, renamed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, FailedCallbackReportsPublishedFileAndBlocksSave) {
  const auto log = log_snapshot();
  const auto old_index = bytes(index);
  const auto filename = storage::normal_entry_filename(storage::EntryType::Log,
                                                       log.id, log.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(filename));
  const fs::path path = fs::path{"logs"} / std::get<std::string>(filename);
  auto stale = log;
  stale.id = id("97183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.insert_entry(
      storage::metadata_from(stale, path))));
  const auto failed = repository.create_log(log, coordinator.log_creation_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed));
  EXPECT_TRUE(std::get<storage::StorageError>(failed).replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(std::get<storage::StorageError>(failed).path));
  EXPECT_FALSE(owned_catalog.find_entry(log.id).has_value());
  // The stored callback detects the stale locator conflict after publication.
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(bytes(index), old_index);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.find_entry(log.id).has_value());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), old_index);
}

TEST_F(NormalRepositoryCallbacksTest, DirectoryCallbackFailureLeavesSavedMetadata) {
  const auto directory = memo_directory();
  const auto prior_index = bytes(index);
  auto stale = directory;
  stale.id = id("a7183823-2574-4bfd-b411-99ed177d3e43");
  const fs::path directory_path = fs::path{"memos"} / directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.insert_directory(
      storage::metadata_from(stale, directory_path))));
  const auto failed = repository.create_directory(core::DirectoryKind::Memo,
                                                  directory, coordinator.directory_creation_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed));
  EXPECT_TRUE(std::get<storage::StorageError>(failed).replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(normal / "memos" / directory.id.to_string() /
                                  ".folio-dir.toml"));
  EXPECT_FALSE(owned_catalog.find_directory(directory.id).has_value());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.find_directory(directory.id).has_value());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(NormalRepositoryCallbacksTest, CallbackOmissionLeavesCatalogUntouched) {
  const auto log = log_snapshot();
  const auto prior_index = bytes(index);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  EXPECT_FALSE(owned_catalog.find_entry(log.id).has_value());
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(fs::exists(normal / "logs"));
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.find_entry(log.id).has_value());
  EXPECT_TRUE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest, BodyOnlyUpdateAndSameNameRenameKeepCatalogPersisted) {
  const auto log = log_snapshot();
  const auto directory = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, directory, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  auto edited = log;
  edited.content = "body only edit";
  const auto path = std::get<storage::LogMetadata>(
      *owned_catalog.find_entry(log.id)).relative_path;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_log(edited, path, coordinator.log_update_callback())));
  EXPECT_FALSE(owned_catalog.unpersisted());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.rename_directory(
      core::DirectoryKind::Memo, directory.id, directory.name, coordinator.directory_update_callback())));
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), prior_index);
  const auto skipped = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(skipped));
  EXPECT_EQ(std::get<storage::SaveOutcome>(skipped), storage::SaveOutcome::SkippedPersisted);
}

TEST_F(NormalRepositoryCallbacksTest, TodoNotesAndSubtasksPreservePersistedMetadata) {
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(todo, coordinator.todo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  const auto path = std::get<storage::TodoMetadata>(
      *owned_catalog.find_entry(todo.id)).relative_path;
  auto edited = todo;
  edited.notes = "Unicode notes: 研究 / café";
  edited.subtasks = {{"Revised subtask", true}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_todo(edited, path, coordinator.todo_update_callback())));
  EXPECT_FALSE(owned_catalog.unpersisted());
  const auto loaded = repository.load_todo(todo.id, path);
  ASSERT_TRUE(std::holds_alternative<core::Todo::Snapshot>(loaded));
  EXPECT_EQ(std::get<core::Todo::Snapshot>(loaded).notes, edited.notes);
  ASSERT_EQ(std::get<core::Todo::Snapshot>(loaded).subtasks.size(), 1U);
  EXPECT_EQ(std::get<core::Todo::Snapshot>(loaded).subtasks.front().content,
            "Revised subtask");
  storage::CatalogQuery query;
  query.text = "Revised subtask";
  EXPECT_TRUE(owned_catalog.query(query).empty());
  query.text = "Unicode notes";
  EXPECT_TRUE(owned_catalog.query(query).empty());
  query.text = "Todo title";
  EXPECT_EQ(owned_catalog.query(query).size(), 1U);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, CallbackObservesCommittedFileAndActualLocator) {
  const auto log = log_snapshot();
  fs::path observed_path;
  bool observed_file = false;
  const storage::PersistedCallback<storage::LogMetadata> observer =
      [&](const storage::LogMetadata& metadata) {
    observed_path = metadata.relative_path;
    observed_file = fs::is_regular_file(normal / observed_path);
    return owned_catalog.insert_entry(metadata);
  };
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log, observer)));
  EXPECT_TRUE(observed_file);
  EXPECT_EQ(observed_path,
            std::get<storage::LogMetadata>(*owned_catalog.find_entry(log.id))
                .relative_path);
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id, observed_path)).content,
            log.content);
}

TEST_F(NormalRepositoryCallbacksTest, ThrowingCallbacksReportSavedDataWithoutRollback) {
  const auto prior_index = bytes(index);
  const storage::PersistedCallback<storage::LogMetadata> standard =
      [](const storage::LogMetadata&)
      -> ::folio::core::internal::Result<std::monostate, storage::StorageError> {
    throw std::runtime_error("synthetic callback exception");
  };
  const auto first = log_snapshot();
  const auto failed_standard = repository.create_log(first, standard);
  expect_code(failed_standard, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed_standard));
  EXPECT_TRUE(std::get<storage::StorageError>(failed_standard).replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(std::get<storage::StorageError>(failed_standard).path));
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(first.id)).content,
            first.content);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);

  const storage::PersistedCallback<storage::LogMetadata> nonstandard =
      [](const storage::LogMetadata&)
      -> ::folio::core::internal::Result<std::monostate, storage::StorageError> {
    throw 7;
  };
  auto second = first;
  second.id = id("87183823-2574-4bfd-b411-99ed177d3e43");
  const auto failed_nonstandard = repository.create_log(second, nonstandard);
  expect_code(failed_nonstandard, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed_nonstandard));
  EXPECT_TRUE(std::get<storage::StorageError>(failed_nonstandard).replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(std::get<storage::StorageError>(failed_nonstandard).path));
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(second.id)).content,
            second.content);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.find_entry(first.id).has_value());
  EXPECT_TRUE(owned_catalog.find_entry(second.id).has_value());
}

TEST_F(NormalRepositoryCallbacksTest, NoOpMoveCorrectsStaleCatalogLocator) {
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  const auto old_path = std::get<storage::MemoMetadata>(
      *owned_catalog.find_entry(memo.id)).relative_path;
  const fs::path renamed = fs::path{"memos"} / "renamed-without-move.md";
  fs::rename(normal / old_path, normal / renamed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, std::nullopt, modified(), coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.find_entry(memo.id))
                .relative_path, renamed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, SavedUpdateWithMissingCatalogIdRequiresRefresh) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  const auto located = repository.locate_entry(storage::EntryType::Log, log.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(located));
  const auto path = std::get<fs::path>(located);
  auto stale = log;
  stale.id = id("97183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.insert_entry(
      storage::metadata_from(stale, path))));
  const auto prior_index = bytes(index);
  auto edited = log;
  edited.title = "Saved before catalog failure";
  edited.modified = modified();
  const auto failed = repository.update_log(edited, path, coordinator.log_update_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failed));
  EXPECT_TRUE(std::get<storage::StorageError>(failed).replacement_completed);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.find_entry(log.id).has_value());
  EXPECT_TRUE(owned_catalog.find_entry(stale.id).has_value());
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id, path)).title,
            edited.title);
  EXPECT_EQ(bytes(index), prior_index);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.find_entry(log.id).has_value());
  EXPECT_FALSE(owned_catalog.find_entry(stale.id).has_value());
}

TEST_F(NormalRepositoryCallbacksTest, FailedRefreshPreservesPersistedInconsistentCatalog) {
  const auto prior_index = bytes(index);
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  const auto located = repository.locate_entry(storage::EntryType::Log, log.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(located));
  auto edited = log;
  edited.title = "Saved without indexed ID";
  edited.modified = modified();
  expect_code(repository.update_log(edited, std::get<fs::path>(located),
                                    coordinator.log_update_callback()),
              storage::StorageErrorCode::PostPersistFailure);
  ASSERT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  ASSERT_TRUE(fs::is_directory(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad Entry"));
  const auto failed = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(failed));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.query().empty());
  EXPECT_EQ(bytes(index), prior_index);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
}

}  // namespace
