#include "error_test_helpers.hpp"
#include <folio/io/filesystem.hpp>
#include "storage/catalog_coordinator.hpp"
#include "storage/normal_repository.hpp"
#include "storage/normal_repository_test_hooks.hpp"
#include "io/internal/atomic_write.hpp"

#ifndef _WIN32
#include <cerrno>
#include <unistd.h>
#endif

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
using folio::testing::diagnostic;
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
  return {id("17183823-2574-4bfd-b411-99ed177d3e43"),
          created(),
          created(),
          "Log title",
          "first body",
          {{std::chrono::year{2027} / 1 / 15}, "UTC", std::nullopt},
          {{"daily"}},
          core::Privacy::Normal};
}

core::Memo::Snapshot memo_snapshot() {
  return {id("27183823-2574-4bfd-b411-99ed177d3e43"),
          created(),
          created(),
          "Memo title",
          "first body",
          {{"research"}},
          std::nullopt,
          core::Privacy::Normal};
}

core::Todo::Snapshot todo_snapshot() {
  return {id("37183823-2574-4bfd-b411-99ed177d3e43"),
          created(),
          created(),
          "Todo title",
          "first notes",
          core::TodoStatus::Pending,
          core::TodoPriority::High,
          core::DueDate{core::DateContext{{std::chrono::year{2028} / 2 / 3}, "Asia/Tokyo", "東京"}},
          std::nullopt,
          core::Privacy::Normal,
          {{"First subtask", false}}};
}

core::Directory::Snapshot memo_directory() {
  return {id("47183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Memo, "研究 / Café"};
}

core::Directory::Snapshot todo_directory() {
  return {id("57183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Todo, "任务"};
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
        const auto candidate = fs::temp_directory_path() / ("folio-repository-callback-test-" +
                                                            core::Id::generate().to_string());
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
      storage::StoragePaths{normal,
                            vault,
                            {state.parent_path() / "config", state.parent_path() / "data", state,
                             cache, state.parent_path() / "logs", std::nullopt}},
      owned_catalog};

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

  static void
  expect_code(const ::folio::core::Result<std::monostate, storage::RepositoryMutationError>& result,
              auto code) {
    ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(result));
    EXPECT_EQ(diagnostic(std::get<storage::RepositoryMutationError>(result)).code, code);
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
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback())));
  const auto log_path =
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).relative_path;
  const auto memo_path =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
          .relative_path;
  const auto todo_path =
      std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
          .relative_path;
  EXPECT_EQ(log_path.parent_path(), fs::path{"logs"});
  EXPECT_EQ(memo_path.parent_path(), fs::path{"memos"});
  EXPECT_EQ(todo_path.parent_path(), fs::path{"todos"});
  EXPECT_EQ(owned_catalog.read_view().query().size(), 3U);
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
  EXPECT_EQ(
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).metadata.title,
      "Edited Log");
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                .metadata.title,
            "Edited Memo");
  EXPECT_EQ(std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
                .metadata.title,
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
      storage::StoragePaths{other_normal,
                            other_vault,
                            {other_state.parent_path() / "config",
                             other_state.parent_path() / "data", other_state, other_cache,
                             other_state.parent_path() / "logs", std::nullopt}},
      other_catalog};
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
  EXPECT_FALSE(fs::exists(other_cache / "index" / "normal.msgpack"));
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
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto persisted_index = bytes(index);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, memo_dir.id, modified(), coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_todo(todo.id, todo_dir.id, modified(), coordinator.todo_update_callback())));
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Memo).records.size(), 0U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Todo).records.size(), 0U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Memo, memo_dir.id))
                .size(),
            1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Todo, todo_dir.id))
                .size(),
            1U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.move_memo(
      memo.id, memo_second.id, modified(), coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Memo, memo_dir.id))
                .size(),
            0U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Memo, memo_second.id))
                .size(),
            1U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.rename_directory(core::DirectoryKind::Memo, memo_second.id, "Renamed / 研究",
                                  coordinator.directory_update_callback())));
  EXPECT_EQ(owned_catalog.read_view().find_directory(memo_second.id)->metadata.name,
            "Renamed / 研究");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, std::nullopt, modified(), coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_todo(todo.id, std::nullopt, modified(), coordinator.todo_update_callback())));
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Memo).records.size(), 1U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Todo).records.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Memo, memo_second.id))
                .size(),
            0U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(core::DirectoryKind::Todo, todo_dir.id))
                .size(),
            0U);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), persisted_index);
}

TEST_F(NormalRepositoryCallbacksTest, RepositoryFailureNotifiesWithoutProvidingSuccessMetadata) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto prior_index = bytes(index);
  int callbacks = 0;
  int failures = 0;
  storage::MutationResultCallback<storage::LogMetadata> observer =
      [&](const storage::MutationNotification<storage::LogMetadata>& notification) {
        if (std::holds_alternative<std::reference_wrapper<const storage::LogMetadata>>(
                notification))
          ++callbacks;
        else
          ++failures;
        return ::folio::core::Result<std::monostate, storage::StorageError>{std::monostate{}};
      };
  expect_code(repository.create_log(log, observer), storage::StorageErrorCode::DuplicateId);
  auto edited = log;
  edited.title = "Blocked edit";
  edited.modified = modified();
  const auto path =
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).relative_path;
  const auto pending =
      normal / path.parent_path() /
      ("." + path.filename().string() + ".tmp-" + core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished"));
  const auto failed = repository.update_log(edited, path, observer);
  expect_code(failed, folio::io::FileErrorCode::RecoveryRequired);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed));
  EXPECT_FALSE(
      diagnostic(std::get<storage::RepositoryMutationError>(failed)).replacement_completed);
  EXPECT_EQ(callbacks, 0);
  EXPECT_EQ(failures, 2);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, StaleLocatorCanBeCorrectedByCallerDiscovery) {
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto prior_index = bytes(index);
  const auto old_path =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
          .relative_path;
  const fs::path renamed = fs::path{"memos"} / "externally-renamed.md";
  fs::rename(normal / old_path, normal / renamed);
  auto edited = memo;
  edited.title = "Discovered edit";
  edited.modified = modified();
  const auto stale = repository.update_memo(edited, old_path, coordinator.memo_update_callback());
  expect_code(stale, storage::StorageErrorCode::StaleLocator);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                .relative_path,
            old_path);
  EXPECT_FALSE(owned_catalog.unpersisted());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_memo(edited, std::nullopt, coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                .relative_path,
            renamed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, FailedCallbackReportsPublishedFileAndBlocksSave) {
  const auto log = log_snapshot();
  const auto old_index = bytes(index);
  const auto filename =
      storage::normal_record_filename(storage::RecordType::Log, log.id, log.created.timestamp);
  ASSERT_TRUE(std::holds_alternative<std::string>(filename));
  const fs::path path = fs::path{"logs"} / std::get<std::string>(filename);
  auto stale = log;
  stale.id = id("97183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.insert_record(storage::metadata_from(stale, path))));
  const auto failed = repository.create_log(log, coordinator.log_creation_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed));
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(failed)).replacement_completed);
  EXPECT_TRUE(
      fs::is_regular_file(diagnostic(std::get<storage::RepositoryMutationError>(failed)).path));
  EXPECT_FALSE(owned_catalog.read_view().find_record(log.id).has_value());
  // The callback reports the stale locator conflict; observing the result flags it.
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(bytes(index), old_index);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().find_record(log.id).has_value());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), old_index);
}

TEST_F(NormalRepositoryCallbacksTest, DirectoryCallbackFailureLeavesSavedMetadata) {
  const auto directory = memo_directory();
  const auto prior_index = bytes(index);
  auto stale = directory;
  stale.id = id("a7183823-2574-4bfd-b411-99ed177d3e43");
  const fs::path directory_path = fs::path{"memos"} / directory.id.to_string();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.insert_directory(storage::metadata_from(stale, directory_path))));
  const auto failed = repository.create_directory(core::DirectoryKind::Memo, directory,
                                                  coordinator.directory_creation_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed));
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(failed)).replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(normal / "memos" / directory.id.to_string() / ".folio-dir.toml"));
  EXPECT_FALSE(owned_catalog.read_view().find_directory(directory.id).has_value());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.read_view().find_directory(directory.id).has_value());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(NormalRepositoryCallbacksTest, CallbackOmissionLeavesCatalogUntouched) {
  const auto log = log_snapshot();
  const auto prior_index = bytes(index);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  EXPECT_FALSE(owned_catalog.read_view().find_record(log.id).has_value());
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(fs::exists(normal / "logs"));
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.read_view().find_record(log.id).has_value());
  EXPECT_TRUE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest, BodyOnlyUpdateAndSameNameRenameKeepCatalogPersisted) {
  const auto log = log_snapshot();
  const auto directory = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_log(log, coordinator.log_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, directory, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  auto edited = log;
  edited.content = "body only edit";
  const auto path =
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).relative_path;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_log(edited, path, coordinator.log_update_callback())));
  EXPECT_FALSE(owned_catalog.unpersisted());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.rename_directory(core::DirectoryKind::Memo, directory.id, directory.name,
                                  coordinator.directory_update_callback())));
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), prior_index);
  const auto skipped = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(skipped));
  EXPECT_EQ(std::get<storage::SaveOutcome>(skipped), storage::SaveOutcome::SkippedPersisted);
}

TEST_F(NormalRepositoryCallbacksTest, TodoNotesAndSubtasksPreservePersistedMetadata) {
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  const auto path = std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
                        .relative_path;
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
  EXPECT_EQ(std::get<core::Todo::Snapshot>(loaded).subtasks.front().content, "Revised subtask");
  storage::RecordQuery query;
  query.text = "Revised subtask";
  EXPECT_TRUE(owned_catalog.read_view().query(query).empty());
  query.text = "Unicode notes";
  EXPECT_TRUE(owned_catalog.read_view().query(query).empty());
  query.text = "Todo title";
  EXPECT_EQ(owned_catalog.read_view().query(query).size(), 1U);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, CallbackObservesCommittedFileAndActualLocator) {
  const auto log = log_snapshot();
  fs::path observed_path;
  bool observed_file = false;
  const storage::MutationResultCallback<storage::LogMetadata> observer =
      [&](const storage::MutationNotification<storage::LogMetadata>& notification) {
        const auto& metadata =
            std::get<std::reference_wrapper<const storage::LogMetadata>>(notification).get();
        observed_path = metadata.relative_path;
        observed_file = fs::is_regular_file(normal / observed_path);
        return owned_catalog.insert_record(metadata);
      };
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log, observer)));
  EXPECT_TRUE(observed_file);
  EXPECT_EQ(
      observed_path,
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).relative_path);
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id, observed_path)).content,
            log.content);
}

TEST_F(NormalRepositoryCallbacksTest, ThrowingCallbacksReportSavedDataWithoutRollback) {
  const auto prior_index = bytes(index);
  const storage::MutationResultCallback<storage::LogMetadata> standard =
      [](const storage::MutationNotification<storage::LogMetadata>&)
      -> ::folio::core::Result<std::monostate, storage::StorageError> {
    throw std::runtime_error("synthetic callback exception");
  };
  const auto first = log_snapshot();
  const auto failed_standard = repository.create_log(first, standard);
  expect_code(failed_standard, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed_standard));
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(failed_standard))
                  .replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(
      diagnostic(std::get<storage::RepositoryMutationError>(failed_standard)).path));
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(first.id)).content, first.content);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);

  const storage::MutationResultCallback<storage::LogMetadata> nonstandard =
      [](const storage::MutationNotification<storage::LogMetadata>&)
      -> ::folio::core::Result<std::monostate, storage::StorageError> { throw 7; };
  auto second = first;
  second.id = id("87183823-2574-4bfd-b411-99ed177d3e43");
  const auto failed_nonstandard = repository.create_log(second, nonstandard);
  expect_code(failed_nonstandard, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed_nonstandard));
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(failed_nonstandard))
                  .replacement_completed);
  EXPECT_TRUE(fs::is_regular_file(
      diagnostic(std::get<storage::RepositoryMutationError>(failed_nonstandard)).path));
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(second.id)).content, second.content);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.read_view().find_record(first.id).has_value());
  EXPECT_TRUE(owned_catalog.read_view().find_record(second.id).has_value());
}

TEST_F(NormalRepositoryCallbacksTest, NoOpMoveCorrectsStaleCatalogLocator) {
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto prior_index = bytes(index);
  const auto old_path =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
          .relative_path;
  const fs::path renamed = fs::path{"memos"} / "renamed-without-move.md";
  fs::rename(normal / old_path, normal / renamed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, std::nullopt, modified(), coordinator.memo_update_callback())));
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                .relative_path,
            renamed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, SavedUpdateWithMissingCatalogIdRequiresRefresh) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  const auto located = repository.locate_record(storage::RecordType::Log, log.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(located));
  const auto path = std::get<fs::path>(located);
  auto stale = log;
  stale.id = id("97183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.insert_record(storage::metadata_from(stale, path))));
  const auto prior_index = bytes(index);
  auto edited = log;
  edited.title = "Saved before catalog failure";
  edited.modified = modified();
  const auto failed = repository.update_log(edited, path, coordinator.log_update_callback());
  expect_code(failed, storage::StorageErrorCode::PostPersistFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(failed));
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(failed)).replacement_completed);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.read_view().find_record(log.id).has_value());
  EXPECT_TRUE(owned_catalog.read_view().find_record(stale.id).has_value());
  EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id, path)).title, edited.title);
  EXPECT_EQ(bytes(index), prior_index);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().find_record(log.id).has_value());
  EXPECT_FALSE(owned_catalog.read_view().find_record(stale.id).has_value());
}

TEST_F(NormalRepositoryCallbacksTest, FailedRefreshPreservesPersistedInconsistentCatalog) {
  const auto prior_index = bytes(index);
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  const auto located = repository.locate_record(storage::RecordType::Log, log.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(located));
  auto edited = log;
  edited.title = "Saved without indexed ID";
  edited.modified = modified();
  expect_code(
      repository.update_log(edited, std::get<fs::path>(located), coordinator.log_update_callback()),
      storage::StorageErrorCode::PostPersistFailure);
  ASSERT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  ASSERT_TRUE(fs::is_directory(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad Entry"));
  const auto failed = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(failed));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_EQ(bytes(index), prior_index);
  const auto blocked = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(std::get<storage::CoordinatorError>(blocked).code,
            storage::CoordinatorErrorCode::Unavailable);
}
// Failure hooks are scoped to one test/thread; production uses the real writer.
struct ScopedRepositoryHooks {
  storage::internal::RepositoryTestHooks previous = storage::internal::repository_test_hooks;
  ~ScopedRepositoryHooks() { storage::internal::repository_test_hooks = previous; }
};

TEST_F(NormalRepositoryCallbacksTest, ManagedPrePublicationFailurePreservesReadyState) {
  auto invalid = log_snapshot();
  invalid.privacy = core::Privacy::Protected;
  const auto result = repository.create_log(invalid, coordinator.log_creation_callback());
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(result));
  EXPECT_FALSE(
      diagnostic(std::get<storage::RepositoryMutationError>(result)).replacement_completed);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::BeforeAuthoritativeChange);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_FALSE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest, NonstandardPreflightExceptionDoesNotPoisonSession) {
  const storage::CreationCheck throwing_check =
      [](const core::Id&) -> core::Result<std::monostate, storage::StorageError> { throw 7; };
  const auto snapshot = log_snapshot();
  const auto result =
      repository.create_log(snapshot, coordinator.log_creation_callback(), throwing_check);
  expect_code(result, storage::StorageErrorCode::OperationFailure);
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(result));
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::BeforeAuthoritativeChange);
  EXPECT_FALSE(fs::exists(normal / "logs"));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(
      snapshot, coordinator.log_creation_callback(), coordinator.creation_check())));
  EXPECT_TRUE(owned_catalog.read_view().contains_entry(snapshot.id));
}

TEST_F(NormalRepositoryCallbacksTest, ProjectionFailureIsClassifiedBeforeSuccessCallbackRuns) {
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.before_projection = [] {
    throw std::runtime_error("synthetic projection failure");
  };
  const auto prior = bytes(index);
  const auto log = log_snapshot();
  const auto result = repository.create_log(log, coordinator.log_creation_callback());
  expect_code(result, storage::StorageErrorCode::PostPersistFailure);
  const auto& error = diagnostic(std::get<storage::RepositoryMutationError>(result));
  EXPECT_TRUE(error.replacement_completed);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::CatalogSynchronizationFailed);
  EXPECT_NE(error.message.find("synthetic projection failure"), std::string::npos);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.shutdown_save()));
  EXPECT_EQ(bytes(index), prior);
  // A failed Refresh cannot remove known inconsistency.
  write_bytes(normal / "logs" / "broken.md", "invalid");
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
  fs::remove(normal / "logs" / "broken.md");
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().find_record(log.id));
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(bytes(index), prior);
}

TEST_F(NormalRepositoryCallbacksTest, ExceptionAfterWriterPublicationReportsUncertainty) {
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view value,
         bool) -> storage::StorageResult<std::monostate> {
    auto result = folio::io::create_file_atomic(path, value);
    if (auto* error = std::get_if<folio::io::FileError>(&result)) return *error;
    throw std::runtime_error("writer exception after publication");
  };
  const auto log = log_snapshot();
  const auto result = repository.create_log(log, coordinator.log_creation_callback());
  expect_code(result, storage::StorageErrorCode::OperationFailure);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
  EXPECT_NE(diagnostic(std::get<storage::RepositoryMutationError>(result))
                .message.find("writer exception after publication"),
            std::string::npos);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<core::Log::Snapshot>(repository.load_log(log.id)));
}

TEST_F(NormalRepositoryCallbacksTest, FailureNotificationPreservesOriginalErrorEvenIfItThrows) {
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.before_projection = [] {
    throw std::runtime_error("projection exception");
  };
  int calls = 0;
  bool failure_seen = false;
  const storage::MutationResultCallback<storage::LogMetadata> callback =
      [&](const storage::MutationNotification<storage::LogMetadata>& notification)
      -> ::folio::core::Result<std::monostate, storage::StorageError> {
    ++calls;
    const auto& error =
        std::get<std::reference_wrapper<const storage::RepositoryMutationError>>(notification)
            .get();
    failure_seen = true;
    EXPECT_EQ(error.stage, storage::MutationFailureStage::CatalogSynchronizationFailed);
    auto handled = coordinator.log_creation_callback()(notification);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(handled));
    throw std::runtime_error("notification exception");
  };
  const auto result = repository.create_log(log_snapshot(), callback);
  expect_code(result, storage::StorageErrorCode::PostPersistFailure);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(failure_seen);
  const auto& error = std::get<storage::RepositoryMutationError>(result);
  EXPECT_NE(diagnostic(error).message.find("projection exception"), std::string::npos);
  EXPECT_EQ(diagnostic(error).message.find("notification exception"), std::string::npos);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

TEST_F(NormalRepositoryCallbacksTest, FailureNotificationReturnErrorCannotReplacePersistenceError) {
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view, bool) -> storage::StorageResult<std::monostate> {
    return folio::io::FileError{folio::io::FileErrorCode::IoError, path,
                                "original metadata write error",
                                std::make_error_code(std::errc::permission_denied)};
  };
  int calls = 0;
  const storage::MutationResultCallback<storage::DirectoryMetadata> callback =
      [&](const storage::MutationNotification<storage::DirectoryMetadata>& notification)
      -> ::folio::core::Result<std::monostate, storage::StorageError> {
    ++calls;
    const auto& error =
        std::get<std::reference_wrapper<const storage::RepositoryMutationError>>(notification)
            .get();
    EXPECT_EQ(error.stage, storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
    (void)coordinator.directory_creation_callback()(notification);
    return storage::StorageError{
        storage::StorageErrorCode::InvalidMetadata, {}, "notification failure", {}};
  };
  const auto dir = memo_directory();
  const auto result = repository.create_directory(dir.kind, dir, callback);
  expect_code(result, folio::io::FileErrorCode::IoError);
  const auto& error = diagnostic(std::get<storage::RepositoryMutationError>(result));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(error.message, "original metadata write error");
  EXPECT_EQ(error.system_error, std::errc::permission_denied);
  EXPECT_EQ(error.path, normal / "memos" / dir.id.to_string() / ".folio-dir.toml");
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest,
       CatalogCallbackFailureAutomaticallyMarksInconsistentWithoutRecursion) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.insert_record(storage::metadata_from(log, fs::path{"logs"} / "stale.md"))));
  owned_catalog.mark_persisted();
  int metadata_calls = 0;
  int failure_calls = 0;
  const storage::MutationResultCallback<storage::LogMetadata> callback =
      [&](const storage::MutationNotification<storage::LogMetadata>& notification) {
        if (std::holds_alternative<std::reference_wrapper<const storage::LogMetadata>>(
                notification))
          ++metadata_calls;
        else
          ++failure_calls;
        return coordinator.log_creation_callback()(notification);
      };
  const auto result = repository.create_log(log, callback);
  expect_code(result, storage::StorageErrorCode::PostPersistFailure);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::CatalogSynchronizationFailed);
  EXPECT_EQ(metadata_calls, 1);
  EXPECT_EQ(failure_calls, 0);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
  // A later successful operation must not certify the rest of the Catalog.
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback())));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.shutdown_save()));
}

TEST_F(NormalRepositoryCallbacksTest,
       IncompleteDirectoryFolderIsReportedWithoutClaimingPublication) {
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view, bool) -> storage::StorageResult<std::monostate> {
    return folio::io::FileError{folio::io::FileErrorCode::IoError, path, "metadata write denied",
                                std::make_error_code(std::errc::permission_denied)};
  };
  const auto dir = memo_directory();
  const auto result =
      repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback());
  expect_code(result, folio::io::FileErrorCode::IoError);
  const auto& error = diagnostic(std::get<storage::RepositoryMutationError>(result));
  EXPECT_FALSE(error.replacement_completed);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
  EXPECT_EQ(error.message, "metadata write denied");
  EXPECT_EQ(error.system_error, std::errc::permission_denied);
  EXPECT_TRUE(fs::is_directory(normal / "memos" / dir.id.to_string()));
  EXPECT_FALSE(owned_catalog.read_view().find_directory(dir.id));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

#ifndef _WIN32
thread_local int sync_calls = 0;
thread_local int failing_sync = 3;
bool injected_sync(int descriptor) {
  if (++sync_calls == failing_sync) {
    errno = EIO;
    return false;
  }
  return ::fsync(descriptor) == 0;
}
auto injected_write(const fs::path& path, std::string_view value, bool create_only)
    -> storage::StorageResult<std::monostate> {
  sync_calls = 0;
  auto result = folio::io::internal::write_file_atomic(path, std::as_bytes(std::span{value}),
                                                       create_only, injected_sync);
  if (auto* error = std::get_if<folio::io::FileError>(&result)) return *error;
  return std::monostate{};
}

TEST_F(NormalRepositoryCallbacksTest,
       AtomicPublicationFailureIsClassifiedForRecordCreationAndUpdate) {
  for (const bool update : {false, true}) {
    ASSERT_TRUE(
        std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
    auto log = log_snapshot();
    if (update) {
      // The failed create iteration already published this Entry.
      ASSERT_TRUE(owned_catalog.read_view().find_record(log.id));
      log.title = "Published update";
    }
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
    const auto old_index = bytes(index);
    ScopedRepositoryHooks hooks;
    failing_sync = 3;
    storage::internal::repository_test_hooks.write = injected_write;
    const auto result =
        update ? repository.update_log(log, std::nullopt, coordinator.log_update_callback())
               : repository.create_log(log, coordinator.log_creation_callback());
    expect_code(result, folio::io::FileErrorCode::DurabilityFailure);
    ASSERT_TRUE(std::holds_alternative<folio::io::FileError>(
        std::get<storage::RepositoryMutationError>(result).cause));
    const auto& error = diagnostic(std::get<storage::RepositoryMutationError>(result));
    EXPECT_TRUE(error.replacement_completed);
    EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
              storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
    EXPECT_EQ(error.system_error, std::errc::io_error);
    EXPECT_NE(error.message.find("Publication completed"), std::string::npos);
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
    EXPECT_FALSE(owned_catalog.unpersisted());
    EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.shutdown_save()));
    EXPECT_EQ(bytes(index), old_index);
    EXPECT_EQ(std::get<core::Log::Snapshot>(repository.load_log(log.id)).title, log.title);
  }
}

TEST_F(NormalRepositoryCallbacksTest, TemporaryLinkCleanupSyncFailureIsClassified) {
  ScopedRepositoryHooks hooks;
  failing_sync = 4;
  storage::internal::repository_test_hooks.write = injected_write;
  const auto result = repository.create_todo(todo_snapshot(), coordinator.todo_creation_callback());
  expect_code(result, folio::io::FileErrorCode::DurabilityFailure);
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(result)).replacement_completed);
  EXPECT_NE(diagnostic(std::get<storage::RepositoryMutationError>(result))
                .message.find("temporary link removal"),
            std::string::npos);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

TEST_F(NormalRepositoryCallbacksTest, MoveDestinationPublicationFailureKeepsSourceAndIsClassified) {
  const auto dir = memo_directory();
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  const auto old_path =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
          .relative_path;
  ScopedRepositoryHooks hooks;
  failing_sync = 3;
  storage::internal::repository_test_hooks.write = injected_write;
  const auto result =
      repository.move_memo(memo.id, dir.id, modified(), coordinator.memo_update_callback());
  expect_code(result, folio::io::FileErrorCode::DurabilityFailure);
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(result)).replacement_completed);
  EXPECT_TRUE(fs::exists(normal / old_path));
  EXPECT_TRUE(fs::exists(normal / "memos" / dir.id.to_string() / old_path.filename()));
  EXPECT_EQ(std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                .relative_path,
            old_path);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

TEST_F(NormalRepositoryCallbacksTest, StagingSyncFailureDoesNotInvalidateCatalog) {
  ScopedRepositoryHooks hooks;
  failing_sync = 1;
  storage::internal::repository_test_hooks.write = injected_write;
  const auto result = repository.create_log(log_snapshot(), coordinator.log_creation_callback());
  expect_code(result, folio::io::FileErrorCode::IoError);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::BeforeAuthoritativeChange);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
}

TEST_F(NormalRepositoryCallbacksTest, DirectoryPublicationFailureIsClassifiedForCreateAndRename) {
  const auto dir = memo_directory();
  for (bool rename : {false, true}) {
    ASSERT_TRUE(
        std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
    ScopedRepositoryHooks hooks;
    failing_sync = 3;
    storage::internal::repository_test_hooks.write = injected_write;
    const auto result =
        rename
            ? repository.rename_directory(dir.kind, dir.id, "Published rename",
                                          coordinator.directory_update_callback())
            : repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback());
    expect_code(result, folio::io::FileErrorCode::DurabilityFailure);
    EXPECT_TRUE(
        diagnostic(std::get<storage::RepositoryMutationError>(result)).replacement_completed);
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
    const auto loaded = repository.load_directory(dir.kind, dir.id);
    ASSERT_TRUE(std::holds_alternative<core::Directory::Snapshot>(loaded));
    EXPECT_EQ(std::get<core::Directory::Snapshot>(loaded).name,
              rename ? "Published rename" : dir.name);
  }
}

TEST_F(NormalRepositoryCallbacksTest, PartialMovesPreserveErrorAndInvalidateCatalog) {
  const auto dir = memo_directory();
  const auto memo = memo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
  const auto old_path =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
          .relative_path;
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.remove_source = [](const fs::path&,
                                                              std::error_code& ec) {
    ec = std::make_error_code(std::errc::permission_denied);
    return false;
  };
  const auto result =
      repository.move_memo(memo.id, dir.id, modified(), coordinator.memo_update_callback());
  expect_code(result, folio::io::FileErrorCode::IoError);
  const auto& error = diagnostic(std::get<storage::RepositoryMutationError>(result));
  EXPECT_TRUE(error.replacement_completed);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
  EXPECT_EQ(error.system_error, std::errc::permission_denied);
  EXPECT_EQ(error.path, normal / old_path);
  EXPECT_TRUE(fs::exists(normal / old_path));
  EXPECT_TRUE(fs::exists(normal / "memos" / dir.id.to_string() / old_path.filename()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

TEST_F(NormalRepositoryCallbacksTest, RemovedSourceSyncFailureCanBeReconciledExplicitly) {
  const auto dir = todo_directory();
  const auto todo = todo_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback())));
  const auto old_path =
      std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
          .relative_path;
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.sync_source = [](int) {
    errno = EIO;
    return false;
  };
  const auto result =
      repository.move_todo(todo.id, dir.id, modified(), coordinator.todo_update_callback());
  expect_code(result, folio::io::FileErrorCode::DurabilityFailure);
  EXPECT_TRUE(diagnostic(std::get<storage::RepositoryMutationError>(result)).replacement_completed);
  EXPECT_FALSE(fs::exists(normal / old_path));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
                .metadata.directory_id,
            dir.id);
  EXPECT_TRUE(owned_catalog.unpersisted());
}
#endif

TEST_F(NormalRepositoryCallbacksTest, ManagedCreationUsesCatalogDespiteUnrelatedMalformedFiles) {
  const auto prior_index = bytes(index);
  fs::create_directories(normal / "logs");
  write_bytes(normal / "logs" / "unrelated.md", "invalid authoritative file");
  const auto& check_creation = coordinator.creation_check();
  const auto memo_dir = memo_directory();
  const auto todo_dir = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      memo_dir.kind, memo_dir, coordinator.directory_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      todo_dir.kind, todo_dir, coordinator.directory_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_log(log_snapshot(), coordinator.log_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(
      memo_snapshot(), coordinator.memo_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(
      todo_snapshot(), coordinator.todo_creation_callback(), check_creation)));
  EXPECT_EQ(owned_catalog.read_view().query().size(), 3u);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(bytes(index), prior_index);
  // Standalone uniqueness/discovery and full Refresh still inspect authoritative files.
  auto other = log_snapshot();
  other.id = core::Id::generate();
  expect_code(repository.create_log(other), storage::StorageErrorCode::CodecError);
  EXPECT_TRUE(std::holds_alternative<storage::StorageError>(
      repository.locate_record(storage::RecordType::Memo, memo_snapshot().id)));
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.refresh_all()));
  EXPECT_EQ(owned_catalog.read_view().query().size(), 3u);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest,
       ManagedCreationRejectsAllEntryIdentityCollisionsBeforePublication) {
  const auto& check_creation = coordinator.creation_check();
  // A Directory identity blocks each Record type before any module is created.
  const auto dir = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.insert_directory(
      storage::metadata_from(dir, fs::path{"memos"} / dir.id.to_string()))));
  owned_catalog.mark_persisted();
  auto log = log_snapshot();
  log.id = dir.id;
  auto memo = memo_snapshot();
  memo.id = dir.id;
  auto todo = todo_snapshot();
  todo.id = dir.id;
  auto check = [&](const auto& result) {
    expect_code(result, storage::StorageErrorCode::DuplicateId);
    EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
              storage::MutationFailureStage::BeforeAuthoritativeChange);
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    EXPECT_FALSE(owned_catalog.unpersisted());
    EXPECT_FALSE(fs::exists(normal));
  };
  check(repository.create_log(log, coordinator.log_creation_callback(), check_creation));
  check(repository.create_memo(memo, coordinator.memo_creation_callback(), check_creation));
  check(repository.create_todo(todo, coordinator.todo_creation_callback(), check_creation));
  check(repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback(),
                                    check_creation));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.remove_directory(dir.id)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.insert_record(storage::metadata_from(log, "logs/renamed.md"))));
  owned_catalog.mark_persisted();
  check(repository.create_directory(dir.kind, dir, coordinator.directory_creation_callback(),
                                    check_creation));
  check(repository.create_memo(memo, coordinator.memo_creation_callback(), check_creation));
  check(repository.create_log(log, coordinator.log_creation_callback(), check_creation));
}

TEST_F(NormalRepositoryCallbacksTest, ManagedPreflightRejectsUninitializedAndInconsistentCatalogs) {
  storage::MetadataCatalog empty;
  storage::CatalogCoordinator unloaded{
      {normal,
       vault,
       {state.parent_path() / "config", state.parent_path() / "data", state, cache,
        state.parent_path() / "logs", std::nullopt}},
      empty};
  expect_code(repository.create_log(log_snapshot(), unloaded.log_creation_callback(),
                                    unloaded.creation_check()),
              storage::StorageErrorCode::CatalogUnavailable);
  EXPECT_FALSE(fs::exists(normal));
  // Existing callback integration supplies the inconsistency notification.
  const storage::RepositoryMutationError failure{
      storage::MutationFailureStage::AuthoritativeStateMayHaveChanged,
      folio::io::FileError{
          folio::io::FileErrorCode::IoError, normal, "synthetic publication failure", {}, true}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      coordinator.log_creation_callback()(std::cref(failure))));
  expect_code(repository.create_log(log_snapshot(), coordinator.log_creation_callback(),
                                    coordinator.creation_check()),
              storage::StorageErrorCode::CatalogUnavailable);
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

TEST_F(NormalRepositoryCallbacksTest, TargetedOperationsIgnoreUnrelatedMalformedRecords) {
  const auto& check_creation = coordinator.creation_check();
  auto log = log_snapshot();
  auto memo = memo_snapshot();
  auto todo = todo_snapshot();
  const auto md = memo_directory();
  const auto td = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_log(log, coordinator.log_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_memo(memo, coordinator.memo_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      md.kind, md, coordinator.directory_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      td.kind, td, coordinator.directory_creation_callback(), check_creation)));
  const auto lp =
      std::get<storage::LogMetadata>(*owned_catalog.read_view().find_record(log.id)).relative_path;
  const auto mp = std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id))
                      .relative_path;
  const auto tp = std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
                      .relative_path;
  // Explicit UUID discovery remains available before the unrelated corruption.
  EXPECT_EQ(std::get<fs::path>(repository.locate_record(storage::RecordType::Memo, memo.id)), mp);
  write_bytes(normal / "logs" / "unrelated.md", "invalid authoritative file");
  const auto prior_index = bytes(index);
  EXPECT_TRUE(std::holds_alternative<core::Log::Snapshot>(repository.load_log(log.id, lp)));
  EXPECT_TRUE(std::holds_alternative<core::Memo::Snapshot>(repository.load_memo(memo.id, mp)));
  EXPECT_TRUE(std::holds_alternative<core::Todo::Snapshot>(repository.load_todo(todo.id, tp)));
  log.title = "updated";
  memo.title = "updated";
  todo.title = "updated";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_log(log, lp, coordinator.log_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_memo(memo, mp, coordinator.memo_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_todo(todo, tp, coordinator.todo_update_callback())));
  ASSERT_TRUE(
      std::holds_alternative<core::Directory::Snapshot>(repository.load_directory(md.kind, md.id)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.rename_directory(
      md.kind, md.id, "new name", coordinator.directory_update_callback())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_memo(memo.id, md.id, modified(), coordinator.memo_update_callback(), mp)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.move_todo(todo.id, td.id, modified(), coordinator.todo_update_callback(), tp)));
  const auto moved_memo =
      std::get<storage::MemoMetadata>(*owned_catalog.read_view().find_record(memo.id));
  const auto moved_todo =
      std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id));
  EXPECT_EQ(moved_memo.relative_path.filename(), mp.filename());
  EXPECT_EQ(moved_todo.relative_path.filename(), tp.filename());
  EXPECT_EQ(moved_memo.metadata.directory_id, md.id);
  EXPECT_EQ(moved_todo.metadata.directory_id, td.id);
  // Old supplied locators fail directly; no rediscovery of the moved files.
  expect_code(repository.move_memo(memo.id, std::nullopt, modified(),
                                   coordinator.memo_update_callback(), mp),
              storage::StorageErrorCode::StaleLocator);
  expect_code(repository.move_todo(todo.id, std::nullopt, modified(),
                                   coordinator.todo_update_callback(), tp),
              storage::StorageErrorCode::StaleLocator);
  expect_code(repository.move_memo(core::Id::generate(), std::nullopt, modified(),
                                   coordinator.memo_update_callback(), moved_memo.relative_path),
              storage::StorageErrorCode::StaleLocator);
  expect_code(repository.move_todo(core::Id::generate(), std::nullopt, modified(),
                                   coordinator.todo_update_callback(), moved_todo.relative_path),
              storage::StorageErrorCode::StaleLocator);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(owned_catalog.read_view().query().size(), 3u);
  EXPECT_EQ(bytes(index), prior_index);
}

TEST_F(NormalRepositoryCallbacksTest, ManagedCreationStillRejectsOccupiedDestinationAtomically) {
  const auto log = log_snapshot();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  const auto path = std::get<fs::path>(repository.locate_record(storage::RecordType::Log, log.id));
  const auto original = bytes(normal / path);
  auto replacement = log;
  replacement.title = "must not replace";
  const auto result = repository.create_log(replacement, coordinator.log_creation_callback(),
                                            coordinator.creation_check());
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(result));
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::BeforeAuthoritativeChange);
  EXPECT_EQ(bytes(normal / path), original);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(NormalRepositoryCallbacksTest, KnownLocatorPartialMoveStillInvalidatesCatalog) {
  const auto dir = todo_directory();
  const auto todo = todo_snapshot();
  const auto& check_creation = coordinator.creation_check();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      dir.kind, dir, coordinator.directory_creation_callback(), check_creation)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_todo(todo, coordinator.todo_creation_callback(), check_creation)));
  const auto source =
      std::get<storage::TodoMetadata>(*owned_catalog.read_view().find_record(todo.id))
          .relative_path;
  const auto prior_index = bytes(index);
  ScopedRepositoryHooks hooks;
  storage::internal::repository_test_hooks.remove_source = [](const fs::path&,
                                                              std::error_code& ec) {
    ec = std::make_error_code(std::errc::permission_denied);
    return false;
  };
  const auto result =
      repository.move_todo(todo.id, dir.id, modified(), coordinator.todo_update_callback(), source);
  expect_code(result, folio::io::FileErrorCode::IoError);
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::AuthoritativeStateMayHaveChanged);
  EXPECT_TRUE(fs::exists(normal / source));
  EXPECT_TRUE(fs::exists(normal / "todos" / dir.id.to_string() / source.filename()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.shutdown_save()));
  EXPECT_EQ(bytes(index), prior_index);
}

} // namespace
