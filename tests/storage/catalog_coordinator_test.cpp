#include "operations/catalog_maintenance.hpp"
#include <folio/io/filesystem.hpp>
#include "storage/catalog_coordinator.hpp"
#include "storage/normal_repository.hpp"
#include "storage/filesystem_durability_test_hooks.hpp"

#include <msgpack.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;
namespace codec = folio::core::storage::index_codec;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      auto candidate = fs::temp_directory_path() /
                       ("folio-index-store-test-" + core::Id::generate().to_string());
      if (fs::create_directory(candidate)) {
        path_ = fs::canonical(candidate);
        return;
      }
    }
    throw std::runtime_error("Could not reserve test directory");
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

core::Id id(const char* value) {
  const auto parsed = core::Id::parse(value);
  if (!parsed) throw std::runtime_error("Invalid test UUID");
  return *parsed;
}

std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not open test file");
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  ASSERT_FALSE(output.fail());
}

template <typename T>
void expect_error(const ::folio::core::Result<T, storage::CoordinatorError>& result,
                  storage::CoordinatorErrorCode code) {
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(result));
  EXPECT_EQ(std::get<storage::CoordinatorError>(result).code, code);
}

storage::MetadataCatalog make_catalog() {
  storage::MetadataCatalog catalog;
  const auto memo_dir = id("87183823-2574-4bfd-b411-99ed177d3e43");
  const auto todo_dir = id("97183823-2574-4bfd-b411-99ed177d3e43");
  if (!std::holds_alternative<std::monostate>(catalog.insert_directory(
          {memo_dir, core::DirectoryKind::Memo, "memos/group", "研究 / Café"})) ||
      !std::holds_alternative<std::monostate>(
          catalog.insert_directory({todo_dir, core::DirectoryKind::Todo, "todos/group", "任务"})))
    throw std::runtime_error("Invalid Directory fixture");
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000000} + std::chrono::nanoseconds{123456000})},
      "Asia/Shanghai", "上海"};
  const auto modified = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000010} + std::chrono::nanoseconds{987654000})},
      "Europe/Paris", std::nullopt};
  const storage::LogMetadata log{{id("17183823-2574-4bfd-b411-99ed177d3e43"),
                                  core::Privacy::Protected,
                                  created,
                                  modified,
                                  "Log 旅行",
                                  {{"daily"}, {"旅行"}},
                                  {{std::chrono::year{2027} / 1 / 15}, "Asia/Tokyo", "東京"}},
                                 "logs/one.md"};
  const storage::MemoMetadata memo{{id("27183823-2574-4bfd-b411-99ed177d3e43"),
                                    core::Privacy::Normal,
                                    created,
                                    modified,
                                    "Memo Café",
                                    {{"research"}},
                                    memo_dir},
                                   "memos/group/one.md"};
  const storage::TodoMetadata todo{
      {id("37183823-2574-4bfd-b411-99ed177d3e43"), core::Privacy::Hidden, created, modified,
       "Todo 日付", core::TodoStatus::Completed, core::TodoPriority::High,
       core::DueDate{core::DateContext{{std::chrono::year{2028} / 2 / 3}, "Asia/Tokyo", "東京"}},
       todo_dir},
      "todos/group/one.toml"};
  const storage::TodoMetadata root_todo{{id("47183823-2574-4bfd-b411-99ed177d3e43"),
                                         core::Privacy::Normal, created, modified, "Root Todo",
                                         core::TodoStatus::Pending, core::TodoPriority::None,
                                         std::nullopt, std::nullopt},
                                        "todos/root.toml"};
  const storage::TodoMetadata timed_todo{
      {id("58183823-2574-4bfd-b411-99ed177d3e43"), core::Privacy::Normal, created, modified,
       "Timed Todo", core::TodoStatus::Pending, core::TodoPriority::Medium,
       core::DueDate{core::TimestampContext{core::Timestamp{std::chrono::seconds{1900000000}},
                                            "UTC", std::nullopt}},
       std::nullopt},
      "todos/timed.toml"};
  for (const storage::RecordMetadata& entry :
       {storage::RecordMetadata{log}, storage::RecordMetadata{memo}, storage::RecordMetadata{todo},
        storage::RecordMetadata{root_todo}, storage::RecordMetadata{timed_todo}}) {
    if (!std::holds_alternative<std::monostate>(catalog.insert_record(entry)))
      throw std::runtime_error("Invalid Entry fixture");
  }
  return catalog;
}

core::Log::Snapshot authoritative_log() {
  const auto created = core::TimestampContext{core::Timestamp{std::chrono::seconds{1800000000}},
                                              "UTC", std::nullopt};
  return {id("a7183823-2574-4bfd-b411-99ed177d3e43"),
          created,
          created,
          "Authoritative Log",
          "synthetic body",
          {{std::chrono::year{2027} / 1 / 15}, "UTC", std::nullopt},
          {{"rebuild"}},
          core::Privacy::Normal};
}

core::Memo::Snapshot authoritative_memo(const core::Id& directory_id) {
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000000} + std::chrono::nanoseconds{123456000})},
      "Asia/Shanghai", "上海"};
  return {id("b7183823-2574-4bfd-b411-99ed177d3e43"),
          created,
          created,
          "研究 Memo",
          "synthetic content",
          {{"research"}},
          directory_id,
          core::Privacy::Normal};
}

core::Todo::Snapshot authoritative_todo(const core::Id& directory_id) {
  const auto created = core::TimestampContext{core::Timestamp{std::chrono::seconds{1800000000}},
                                              "UTC", std::nullopt};
  return {id("c7183823-2574-4bfd-b411-99ed177d3e43"),
          created,
          created,
          "旅行 Todo",
          "synthetic notes",
          core::TodoStatus::Pending,
          core::TodoPriority::High,
          core::DueDate{core::DateContext{{std::chrono::year{2028} / 3 / 4}, "Asia/Tokyo", "東京"}},
          directory_id,
          core::Privacy::Normal,
          {{"Book train", false}}};
}

void change_log_title(storage::MetadataCatalog& catalog) {
  const auto found = catalog.read_view().find_record(id("17183823-2574-4bfd-b411-99ed177d3e43"));
  if (!found) throw std::runtime_error("Missing test Log");
  auto log = std::get<storage::LogMetadata>(*found);
  log.metadata.title = "Changed after first save";
  if (!std::holds_alternative<std::monostate>(catalog.update_record(storage::RecordMetadata{log})))
    throw std::runtime_error("Could not update test Log");
}

class CatalogCoordinatorTest : public testing::Test {
protected:
  TemporaryDirectory temporary;
  fs::path normal = temporary.path() / "normal";
  fs::path vault = temporary.path() / "vault";
  fs::path state = temporary.path() / "custom-state";
  fs::path cache = temporary.path() / "cache";
  fs::path file = state / "index" / "normal.msgpack";

  storage::MetadataCatalog owned_catalog;

  storage::StoragePaths paths() const {
    return {normal,
            vault,
            {temporary.path() / "config", temporary.path() / "data", state, cache,
             state.parent_path() / "logs", std::nullopt}};
  }
  void seed(const storage::MetadataCatalog& catalog) const {
    const auto encoded = codec::encode_index(catalog);
    ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
    ASSERT_TRUE(fs::create_directories(file.parent_path()) || fs::is_directory(file.parent_path()));
    ASSERT_NO_FATAL_FAILURE(write_bytes(file, std::get<std::string>(encoded)));
  }
  void expect_no_data_roots() const {
    EXPECT_FALSE(fs::exists(normal));
    EXPECT_FALSE(fs::exists(vault));
    EXPECT_FALSE(fs::exists(cache / "index"));
  }
};

TEST_F(CatalogCoordinatorTest, ClearingDisposableCachePreservesStateIndex) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  fs::create_directories(cache);
  write_bytes(cache / "disposable", "temporary");
  fs::remove_all(cache);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto loaded = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(loaded).index_error);
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
}

TEST_F(CatalogCoordinatorTest, MaintenancePreparesIndexWithoutPublishingOrEstablishingSession) {
  seed(make_catalog());
  folio::application::internal::CatalogMaintenance maintenance{paths()};
  auto prepared = maintenance.prepare_initialization();
  using Prepared = folio::application::internal::CatalogMaintenance::PreparedInitialization;
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto& candidate = std::get<Prepared>(prepared);
  EXPECT_EQ(candidate.catalog.read_view().query().size(), 5U);
  EXPECT_FALSE(candidate.catalog.unpersisted());
  EXPECT_FALSE(candidate.outcome.index_error);
  EXPECT_FALSE(candidate.outcome.previous_session_incomplete);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_FALSE(fs::exists(state / "session.marker"));
}

TEST_F(CatalogCoordinatorTest, MaintenancePreparesFallbackWithoutPublishingOrEstablishingSession) {
  folio::application::internal::CatalogMaintenance maintenance{paths()};
  auto prepared = maintenance.prepare_initialization();
  using Prepared = folio::application::internal::CatalogMaintenance::PreparedInitialization;
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto& candidate = std::get<Prepared>(prepared);
  ASSERT_TRUE(candidate.outcome.index_error);
  EXPECT_EQ(candidate.outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
  EXPECT_FALSE(candidate.outcome.previous_session_incomplete);
  EXPECT_TRUE(candidate.catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_FALSE(fs::exists(state / "session.marker"));
}

TEST_F(CatalogCoordinatorTest, SharedApplicationParentKeepsIndexInEffectiveState) {
  auto effective = paths();
  effective.application.config = temporary.path() / "application";
  effective.application.data = effective.application.config;
  effective.application.state = effective.application.data / "state";
  effective.application.cache = effective.application.data / "cache";
  const auto expected = effective.application.state / "index/normal.msgpack";
  {
    storage::CatalogCoordinator coordinator{effective, owned_catalog};
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
    EXPECT_TRUE(owned_catalog.unpersisted());
    EXPECT_FALSE(fs::exists(expected));
    const auto saved = coordinator.finish_session();
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
    EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
    EXPECT_FALSE(owned_catalog.unpersisted());
  }
  EXPECT_TRUE(fs::is_regular_file(expected));
  EXPECT_FALSE(fs::exists(effective.application.cache / "index"));
  EXPECT_FALSE(fs::exists(cache / "index"));
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(vault));
  storage::CatalogCoordinator restored{effective, owned_catalog};
  const auto loaded = restored.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(loaded).index_error);
  EXPECT_FALSE(owned_catalog.unpersisted());
}

TEST_F(CatalogCoordinatorTest, FormerCacheIndexIsIgnoredAndLeftUntouched) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  const auto locator = repository.locate_record(storage::RecordType::Log, snapshot.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto entry_path = normal / std::get<fs::path>(locator);
  const auto entry_bytes = read_bytes(entry_path);
  const auto legacy = cache / "index/normal.msgpack";
  ASSERT_TRUE(fs::create_directories(legacy.parent_path()));
  const auto stale = codec::encode_index(make_catalog());
  ASSERT_TRUE(std::holds_alternative<std::string>(stale));
  // Neither a valid but stale Index nor an invalid old file may be imported.
  for (const auto& bytes : {std::get<std::string>(stale), std::string{"invalid old Index"}}) {
    ASSERT_NO_FATAL_FAILURE(write_bytes(legacy, bytes));
    storage::MetadataCatalog catalog;
    storage::CatalogCoordinator coordinator{paths(), catalog};
    const auto loaded = coordinator.initialize();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
    const auto& outcome = std::get<storage::InitializationOutcome>(loaded);
    ASSERT_TRUE(outcome.index_error);
    EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
    EXPECT_EQ(outcome.index_error->path, file);
    EXPECT_EQ(catalog.read_view().query().size(), 1U);
    EXPECT_TRUE(catalog.read_view().find_record(snapshot.id));
    EXPECT_TRUE(catalog.unpersisted());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(read_bytes(legacy), bytes);
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
    EXPECT_TRUE(fs::is_regular_file(file));
    EXPECT_FALSE(catalog.unpersisted());
    EXPECT_EQ(read_bytes(legacy), bytes);
    EXPECT_EQ(read_bytes(entry_path), entry_bytes);
    fs::remove_all(state / "index");
  }
}

TEST_F(CatalogCoordinatorTest, ValidCacheIgnoresFormerCacheIndexWithoutScanning) {
  seed(make_catalog());
  const auto legacy = cache / "index/normal.msgpack";
  ASSERT_TRUE(fs::create_directories(legacy.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(legacy, "invalid old Index"));
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs/broken.md", "invalid Entry"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto loaded = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(loaded).index_error);
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(legacy), "invalid old Index");
}

TEST_F(CatalogCoordinatorTest, CacheRemovalBetweenSessionsRebuildsWithoutEntryLoss) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  const auto locator = repository.locate_record(storage::RecordType::Log, snapshot.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto entry_path = normal / std::get<fs::path>(locator);
  const auto entry_bytes = read_bytes(entry_path);
  // Exercise both a removed Cache directory and an existing empty directory.
  for (const bool empty_directory : {false, true}) {
    fs::remove_all(state / "index");
    if (empty_directory) {
      ASSERT_TRUE(fs::create_directories(state / "index"));
    }
    storage::MetadataCatalog catalog;
    storage::CatalogCoordinator coordinator{paths(), catalog};
    const auto loaded = coordinator.initialize();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
    ASSERT_TRUE(std::get<storage::InitializationOutcome>(loaded).index_error);
    EXPECT_EQ(std::get<storage::InitializationOutcome>(loaded).index_error->code,
              storage::CoordinatorErrorCode::Missing);
    EXPECT_TRUE(catalog.read_view().find_record(snapshot.id));
    EXPECT_TRUE(catalog.unpersisted());
    EXPECT_FALSE(fs::exists(file));
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
    EXPECT_TRUE(fs::is_regular_file(file));
    EXPECT_EQ(read_bytes(entry_path), entry_bytes);
    EXPECT_FALSE(fs::exists(cache / "index"));
  }
}

TEST_F(CatalogCoordinatorTest, InvalidCacheObjectsAreFatalWithoutReconstruction) {
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs/broken.md", "invalid Entry"));
  // Each path conflict must be reported, rather than treated as cache loss.
  for (const auto& target : {file.parent_path(), file}) {
    fs::remove_all(state / "index");
    fs::create_directories(target.parent_path());
    if (target == file) {
      ASSERT_TRUE(fs::create_directory(target));
    } else {
      ASSERT_NO_FATAL_FAILURE(write_bytes(target, "not a directory"));
    }
    storage::MetadataCatalog catalog;
    storage::CatalogCoordinator coordinator{paths(), catalog};
    expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::InvalidPath);
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
    EXPECT_TRUE(catalog.read_view().query().empty());
    expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  }
}

TEST_F(CatalogCoordinatorTest, BorrowedCatalogSurvivesCoordinatorWithoutConstructorIo) {
  owned_catalog = make_catalog();
  const auto before = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(before));
  {
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
    EXPECT_FALSE(fs::exists(normal));
    EXPECT_FALSE(fs::exists(vault));
    EXPECT_FALSE(fs::exists(cache / "index"));
    EXPECT_FALSE(fs::exists(cache));
    EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  }
  const auto after = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(after));
  EXPECT_EQ(std::get<std::string>(after), std::get<std::string>(before));
  EXPECT_TRUE(owned_catalog.unpersisted());
}

TEST_F(CatalogCoordinatorTest, FailedStartupNeverOverwritesCache) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "corrupt cache"));
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad Entry"));
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  auto catalog = make_catalog();
  owned_catalog = std::move(catalog);
  EXPECT_TRUE(owned_catalog.unpersisted());
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(read_bytes(file), "corrupt cache");
  EXPECT_EQ(read_bytes(normal / "logs" / "malformed.md"), "bad Entry");
  EXPECT_FALSE(fs::exists(vault));
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, ShutdownBeforeStartupDoesNotBlockStartup) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
  seed(make_catalog());
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::SkippedPersisted);
}

TEST_F(CatalogCoordinatorTest, PersistedLoadedCatalogSkipsShutdownIoAndQueriesMemory) {
  const auto original = make_catalog();
  seed(original);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Log).records.size(), 1U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Memo).directories.size(), 1U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Todo).directories.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(
                    core::DirectoryKind::Memo, id("87183823-2574-4bfd-b411-99ed177d3e43")))
                .size(),
            1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
                owned_catalog.read_view().list_directory(
                    core::DirectoryKind::Todo, id("97183823-2574-4bfd-b411-99ed177d3e43")))
                .size(),
            1U);
  const auto encoded = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(std::get<std::string>(encoded), read_bytes(file));
  ASSERT_TRUE(fs::remove(file));
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::SkippedPersisted);
  EXPECT_FALSE(fs::exists(file));
  const auto again = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(again));
  EXPECT_EQ(std::get<storage::SaveOutcome>(again), storage::SaveOutcome::SkippedPersisted);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, UnpersistedLoadedCatalogReplacesCacheAndAcknowledgesSave) {
  seed(make_catalog());
  const auto before = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  ASSERT_TRUE(owned_catalog.unpersisted());
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::Saved);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_NE(read_bytes(file), before);
  const auto encoded = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(read_bytes(file), std::get<std::string>(encoded));
  const auto again = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(again));
  EXPECT_EQ(std::get<storage::SaveOutcome>(again), storage::SaveOutcome::SkippedPersisted);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, UnpersistedEmptyCatalogCanReplaceAndMissingTargetCanBeCreated) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  for (const auto& entry : owned_catalog.read_view().query()) {
    EXPECT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.remove_record(
        std::visit([](const auto& value) { return value.metadata.id; }, entry))));
  }
  // Directory records must be removed after their members.
  for (const auto kind : {core::DirectoryKind::Memo, core::DirectoryKind::Todo}) {
    for (const auto& directory : owned_catalog.read_view().list_directories(kind))
      EXPECT_TRUE(std::holds_alternative<std::monostate>(
          owned_catalog.remove_directory(directory.metadata.id)));
  }
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  ASSERT_TRUE(fs::remove(file));
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(owned_catalog.unpersisted());
  const auto decoded = codec::decode_index(read_bytes(file));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  EXPECT_TRUE(std::get<storage::MetadataCatalog>(decoded).read_view().query().empty());
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, StartupCacheErrorsReportFallbackReasonsAndCannotRetry) {
  auto check = [&](storage::CoordinatorErrorCode expected) {
    // Independent cache-error fixtures, not consecutive abandoned sessions.
    fs::remove(state / "session.marker");
    const auto before = fs::exists(file) ? read_bytes(file) : std::string{};
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    const auto started = coordinator.initialize();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(started));
    const auto& outcome = std::get<storage::InitializationOutcome>(started);
    ASSERT_TRUE(outcome.index_error.has_value());
    EXPECT_EQ(outcome.index_error->code, expected);
    if (expected == storage::CoordinatorErrorCode::Corrupt ||
        expected == storage::CoordinatorErrorCode::UnsupportedVersion)
      EXPECT_TRUE(outcome.index_error->codec_error.has_value());
    EXPECT_TRUE(outcome.diagnostics.empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    EXPECT_TRUE(owned_catalog.unpersisted());
    EXPECT_EQ(fs::exists(file) ? read_bytes(file) : std::string{}, before);
    expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  };
  check(storage::CoordinatorErrorCode::Missing);
  EXPECT_FALSE(fs::exists(cache / "index"));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "bad MessagePack"));
  check(storage::CoordinatorErrorCode::Corrupt);
  const auto valid = codec::encode_index(make_catalog());
  ASSERT_TRUE(std::holds_alternative<std::string>(valid));
  auto truncated_bytes = std::get<std::string>(valid);
  truncated_bytes.resize(truncated_bytes.size() / 2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, truncated_bytes));
  check(storage::CoordinatorErrorCode::Corrupt);

  msgpack::sbuffer packed;
  msgpack::packer<msgpack::sbuffer> packer{packed};
  packer.pack_map(3);
  packer.pack(std::string{"directories"});
  packer.pack_array(0);
  packer.pack(std::string{"entries"});
  packer.pack_array(0);
  packer.pack(std::string{"schema_version"});
  packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  check(storage::CoordinatorErrorCode::UnsupportedVersion);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, InvalidRootsAndIoFailureDoNotCreateCache) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::MetadataCatalog overlap_catalog;
  storage::CatalogCoordinator overlap{invalid, overlap_catalog};
  expect_error(overlap.initialize(), storage::CoordinatorErrorCode::InvalidPath);
  expect_error(overlap.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  invalid = paths();
  invalid.application.state = temporary.path() / std::string(8192, 'x');
  storage::MetadataCatalog io_catalog;
  storage::CatalogCoordinator io{invalid, io_catalog};
  const auto failure = io.initialize();
  expect_error(failure, storage::CoordinatorErrorCode::IoError);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failure).file_error.has_value());
  EXPECT_TRUE(std::get<storage::CoordinatorError>(failure).file_error->system_error);
  EXPECT_FALSE(fs::exists(cache / "index"));
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, EncodingFailurePreservesExistingCacheAndUnpersistedCatalog) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  auto log = std::get<storage::LogMetadata>(
      *owned_catalog.read_view().find_record(id("17183823-2574-4bfd-b411-99ed177d3e43")));
  log.metadata.log_date.date = std::chrono::year{-1} / 1 / 1;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.update_record(storage::RecordMetadata{log})));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::CodecError);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failed).codec_error.has_value());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(owned_catalog.unpersisted());
  log.metadata.log_date.date = std::chrono::year{2027} / 1 / 15;
  log.metadata.title = "Recovered after failed encoding";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      owned_catalog.update_record(storage::RecordMetadata{log})));
  const auto retried = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(retried));
  EXPECT_EQ(std::get<storage::SaveOutcome>(retried), storage::SaveOutcome::Saved);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_NE(read_bytes(file), original);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, PendingTemporaryBlocksReplacementWithoutNewTemp) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  const auto pending = file.parent_path() / ("." + file.filename().string() + ".tmp-" +
                                             core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished cache"));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  const auto& cause = std::get<storage::CoordinatorError>(failed);
  ASSERT_TRUE(cause.file_error.has_value());
  EXPECT_EQ(cause.file_error->code, folio::io::FileErrorCode::RecoveryRequired);
  EXPECT_FALSE(cause.file_error->replacement_completed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_EQ(read_bytes(pending), "unfinished cache");
  EXPECT_EQ(std::distance(fs::directory_iterator(file.parent_path()), fs::directory_iterator{}), 2);
  ASSERT_TRUE(fs::remove(pending));
  const auto retried = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(retried));
  EXPECT_EQ(std::get<storage::SaveOutcome>(retried), storage::SaveOutcome::Saved);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_NE(read_bytes(file), original);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, ParentFileAndSymlinksPreventShutdownWrite) {
  seed(make_catalog());
  storage::MetadataCatalog parent_catalog;
  storage::CatalogCoordinator parent_file{paths(), parent_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(parent_file.initialize()));
  change_log_title(parent_catalog);
  ASSERT_TRUE(fs::remove(file));
  ASSERT_TRUE(fs::remove(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file.parent_path(), "not a directory"));
  const auto blocked = parent_file.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(read_bytes(file.parent_path()), "not a directory");
  EXPECT_TRUE(parent_catalog.unpersisted());
  ASSERT_TRUE(fs::remove(file.parent_path()));

  // Independent unsafe-target case: seed a valid Index and reset session state.
  fs::remove(state / "session.marker");
  seed(make_catalog());
  storage::MetadataCatalog symlink_catalog;
  storage::CatalogCoordinator symlinked{paths(), symlink_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(symlinked.initialize()));
  change_log_title(symlink_catalog);
  const auto outside = temporary.path() / "outside.msgpack";
  ASSERT_NO_FATAL_FAILURE(write_bytes(outside, "outside bytes"));
  ASSERT_TRUE(fs::remove(file));
  std::error_code ec;
  fs::create_symlink(outside, file, ec);
  if (ec) GTEST_SKIP() << "Cannot create file symlink: " << ec.message();
  expect_error(symlinked.shutdown_save(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(read_bytes(outside), "outside bytes");
  EXPECT_TRUE(symlink_catalog.unpersisted());
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, SymlinkedIndexParentCannotRedirectShutdownWrite) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  ASSERT_TRUE(fs::remove(file));
  ASSERT_TRUE(fs::remove(file.parent_path()));
  const auto outside = temporary.path() / "outside";
  ASSERT_TRUE(fs::create_directory(outside));
  std::error_code ec;
  fs::create_directory_symlink(outside, file.parent_path(), ec);
  if (ec) GTEST_SKIP() << "Cannot create directory symlink: " << ec.message();
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_TRUE(fs::is_empty(outside));
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, MissingIndexRebuildsAuthoritativeFilesWithoutIndexWrite) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto started = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(started));
  ASSERT_TRUE(std::get<storage::InitializationOutcome>(started).index_error.has_value());
  EXPECT_EQ(std::get<storage::InitializationOutcome>(started).index_error->code,
            storage::CoordinatorErrorCode::Missing);
  ASSERT_FALSE(fs::exists(file));
  const auto rebuilt = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(rebuilt));
  EXPECT_TRUE(std::get<std::vector<storage::ExcludedObject>>(rebuilt).empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto found = owned_catalog.read_view().find_record(snapshot.id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(std::get<storage::LogMetadata>(*found).metadata.title, snapshot.title);
  EXPECT_EQ(std::get<storage::LogMetadata>(*found).metadata.created.timestamp,
            snapshot.created.timestamp);
  EXPECT_FALSE(fs::exists(cache / "index"));
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(vault));
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, CorruptIndexCanRebuildAndReplacesItOnlyOnSave) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "corrupt index"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto started = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(started));
  ASSERT_TRUE(std::get<storage::InitializationOutcome>(started).index_error.has_value());
  EXPECT_EQ(std::get<storage::InitializationOutcome>(started).index_error->code,
            storage::CoordinatorErrorCode::Corrupt);
  const auto old_bytes = read_bytes(file);
  const auto rebuilt = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(rebuilt));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(file), old_bytes);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_NE(read_bytes(file), old_bytes);
}

TEST_F(CatalogCoordinatorTest, FailedRefreshPreservesReadyCatalogAndUnpersistedFlag) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  const auto before = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(before));
  const auto old_file = read_bytes(file);
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad front matter"));
  const auto failed = coordinator.refresh_all();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failed).storage_error.has_value());
  EXPECT_EQ(std::get<storage::CoordinatorError>(failed).storage_error->code,
            storage::StorageErrorCode::CodecError);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto after = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(after));
  EXPECT_EQ(std::get<std::string>(after), std::get<std::string>(before));
  EXPECT_EQ(read_bytes(file), old_file);
}

TEST_F(CatalogCoordinatorTest, EmptyRebuildEstablishesReadyUnpersistedCatalog) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto rebuilt = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(rebuilt));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(cache / "index"));
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_TRUE(fs::is_regular_file(file));
}

TEST_F(CatalogCoordinatorTest, RebuildReportsDiagnosticsWithoutIndexIo) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.txt", "ignored"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto rebuilt = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(rebuilt));
  const auto& diagnostics = std::get<std::vector<storage::ExcludedObject>>(rebuilt);
  ASSERT_EQ(diagnostics.size(), 1U);
  EXPECT_FALSE(diagnostics.front().path.empty());
  EXPECT_EQ(owned_catalog.read_view().query().size(), 1U);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, RebuildBeforeStartupPreventsCacheOverwriteByStartupLoad) {
  seed(make_catalog());
  const auto cache_bytes = read_bytes(file);
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.read_view().find_record(snapshot.id).has_value());
  EXPECT_FALSE(owned_catalog.read_view()
                   .find_record(id("17183823-2574-4bfd-b411-99ed177d3e43"))
                   .has_value());
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(file), cache_bytes);
}

TEST_F(CatalogCoordinatorTest, StartupLoadsValidIndexWithoutScanningNormalFiles) {
  seed(make_catalog());
  const auto old_bytes = read_bytes(file);
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "not a Log"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  EXPECT_FALSE(outcome.index_error.has_value());
  EXPECT_TRUE(outcome.diagnostics.empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  EXPECT_EQ(read_bytes(file), old_bytes);
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
}

TEST_F(CatalogCoordinatorTest, StartupFallsBackForMissingCorruptAndUnsupportedIndex) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  auto verify_fallback = [&](storage::CoordinatorErrorCode expected) {
    // Reset session evidence between independent cache-error cases.
    fs::remove(state / "session.marker");
    const auto previous = fs::exists(file) ? read_bytes(file) : std::string{};
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    const auto result = coordinator.initialize();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(result));
    const auto& outcome = std::get<storage::InitializationOutcome>(result);
    ASSERT_TRUE(outcome.index_error.has_value());
    EXPECT_EQ(outcome.index_error->code, expected);
    EXPECT_TRUE(outcome.diagnostics.empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    EXPECT_TRUE(owned_catalog.unpersisted());
    EXPECT_TRUE(owned_catalog.read_view().find_record(snapshot.id).has_value());
    EXPECT_EQ(fs::exists(file) ? read_bytes(file) : std::string{}, previous);
    expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  };
  verify_fallback(storage::CoordinatorErrorCode::Missing);
  EXPECT_FALSE(fs::exists(cache / "index"));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "bad MessagePack"));
  verify_fallback(storage::CoordinatorErrorCode::Corrupt);
  msgpack::sbuffer packed;
  msgpack::packer<msgpack::sbuffer> packer{packed};
  packer.pack_map(3);
  packer.pack(std::string{"directories"});
  packer.pack_array(0);
  packer.pack(std::string{"entries"});
  packer.pack_array(0);
  packer.pack(std::string{"schema_version"});
  packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  verify_fallback(storage::CoordinatorErrorCode::UnsupportedVersion);
}

TEST_F(CatalogCoordinatorTest, StartupFallbackRestoresMixedMetadataAndDirectoryMembership) {
  storage::NormalRepository repository{normal};
  const core::Directory::Snapshot memo_directory{id("d7183823-2574-4bfd-b411-99ed177d3e43"),
                                                 core::DirectoryKind::Memo, "研究 / Café"};
  const core::Directory::Snapshot todo_directory{id("e7183823-2574-4bfd-b411-99ed177d3e43"),
                                                 core::DirectoryKind::Todo, "旅行"};
  const auto log = authoritative_log();
  const auto memo = authoritative_memo(memo_directory.id);
  const auto todo = authoritative_todo(todo_directory.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, memo_directory)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Todo, todo_directory)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(todo)));
  auto memo_path = repository.locate_record(storage::RecordType::Memo, memo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_path));
  const auto renamed = fs::path{"memos"} / memo_directory.id.to_string() / "renamed.md";
  fs::rename(normal / std::get<fs::path>(memo_path), normal / renamed);
  const auto log_path = repository.locate_record(storage::RecordType::Log, log.id);
  const auto todo_path = repository.locate_record(storage::RecordType::Todo, todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(log_path));
  ASSERT_TRUE(std::holds_alternative<fs::path>(todo_path));
  storage::MetadataCatalog expected;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_directory(
      storage::metadata_from(memo_directory, fs::path{"memos"} / memo_directory.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_directory(
      storage::metadata_from(todo_directory, fs::path{"todos"} / todo_directory.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      expected.insert_record(storage::metadata_from(log, std::get<fs::path>(log_path)))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      expected.insert_record(storage::metadata_from(memo, renamed))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      expected.insert_record(storage::metadata_from(todo, std::get<fs::path>(todo_path)))));
  const auto expected_bytes = codec::encode_index(expected);
  ASSERT_TRUE(std::holds_alternative<std::string>(expected_bytes));

  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  ASSERT_TRUE(std::get<storage::InitializationOutcome>(initialized).index_error.has_value());
  EXPECT_EQ(std::get<storage::InitializationOutcome>(initialized).index_error->code,
            storage::CoordinatorErrorCode::Missing);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto actual_bytes = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(actual_bytes));
  EXPECT_EQ(std::get<std::string>(actual_bytes), std::get<std::string>(expected_bytes));
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Log).records.size(), 1U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Memo).records.size(), 0U);
  EXPECT_EQ(owned_catalog.read_view().list_root(storage::RecordType::Todo).records.size(), 0U);
  EXPECT_EQ(
      std::get<std::vector<storage::RecordMetadata>>(
          owned_catalog.read_view().list_directory(core::DirectoryKind::Memo, memo_directory.id))
          .size(),
      1U);
  EXPECT_EQ(
      std::get<std::vector<storage::RecordMetadata>>(
          owned_catalog.read_view().list_directory(core::DirectoryKind::Todo, todo_directory.id))
          .size(),
      1U);
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, StartupReturnsExclusionsFromFallbackAndSavesOnlyLater) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.txt", "ignored"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  ASSERT_TRUE(outcome.index_error.has_value());
  EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
  ASSERT_EQ(outcome.diagnostics.size(), 1U);
  EXPECT_FALSE(outcome.diagnostics.front().path.empty());
  EXPECT_FALSE(fs::exists(cache / "index"));
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
}

TEST_F(CatalogCoordinatorTest, StartupMissingIndexAndEmptyNormalRootBuildsEmptyReadyCatalog) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  ASSERT_TRUE(outcome.index_error.has_value());
  EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
  EXPECT_TRUE(outcome.diagnostics.empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, StartupPropagatesPathAndIoErrorsWithoutScanning) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::MetadataCatalog overlap_catalog;
  storage::CatalogCoordinator overlap{invalid, overlap_catalog};
  expect_error(overlap.initialize(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(overlap.status(), storage::CatalogStatus::Unavailable);
  EXPECT_FALSE(fs::exists(normal));
  invalid = paths();
  invalid.application.state = temporary.path() / std::string(8192, 'x');
  storage::MetadataCatalog io_catalog;
  storage::CatalogCoordinator io{invalid, io_catalog};
  expect_error(io.initialize(), storage::CoordinatorErrorCode::IoError);
  EXPECT_EQ(io.status(), storage::CatalogStatus::Unavailable);
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(cache / "index"));
}

TEST_F(CatalogCoordinatorTest, FailedFallbackScanPreservesCatalogAndUnavailableStatus) {
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad front matter"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  owned_catalog = make_catalog();
  const auto before = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(before));
  const auto failed = coordinator.initialize();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto after = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(after));
  EXPECT_EQ(std::get<std::string>(after), std::get<std::string>(before));
  EXPECT_FALSE(fs::exists(cache / "index"));
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
}

TEST_F(CatalogCoordinatorTest, FailedRefreshPreservesPersistedCatalogAndReadyStatus) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto original = read_bytes(file);
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad front matter"));
  const auto failed = coordinator.refresh_all();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
  EXPECT_EQ(read_bytes(file), original);
}

TEST_F(CatalogCoordinatorTest, ExplicitRefreshReplacesCatalogWithoutIndexIo) {
  seed(make_catalog());
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.txt", "ignored"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "corrupt after initialization"));
  const auto cached_bytes = read_bytes(file);
  const auto refreshed = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(refreshed));
  EXPECT_EQ(std::get<std::vector<storage::ExcludedObject>>(refreshed).size(), 1U);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.read_view().find_record(snapshot.id).has_value());
  EXPECT_FALSE(owned_catalog.read_view()
                   .find_record(id("17183823-2574-4bfd-b411-99ed177d3e43"))
                   .has_value());
  EXPECT_EQ(read_bytes(file), cached_bytes);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_NE(read_bytes(file), cached_bytes);
}

#ifndef _WIN32
TEST_F(CatalogCoordinatorTest, DirectoryLockFailurePreservesExistingCacheAndUnpersistedFlag) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  const int descriptor = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) GTEST_SKIP() << "Cannot open cache directory for locking";
  struct LockGuard {
    int fd;
    ~LockGuard() {
      (void)flock(fd, LOCK_UN);
      (void)close(fd);
    }
  } guard{descriptor};
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    GTEST_SKIP() << "Cannot acquire cache directory lock";
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::IoError);
  const auto& cause = std::get<storage::CoordinatorError>(failed);
  ASSERT_TRUE(cause.file_error.has_value());
  EXPECT_EQ(cause.file_error->code, folio::io::FileErrorCode::IoError);
  EXPECT_FALSE(cause.file_error->replacement_completed);
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(owned_catalog.unpersisted());
  expect_no_data_roots();
}
#endif

// Session tests simulate abandoned owners rather than delivering real power
// loss. They verify persisted ordering and failures, not disk-controller behavior.
TEST_F(CatalogCoordinatorTest, CleanStartupEstablishesMarkerAndReadOnlyFinishDoesNotRewriteIndex) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  const auto modified = fs::last_write_time(file);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  EXPECT_FALSE(fs::exists(state / "session.marker")); // Construction is side-effect free.
  const auto loaded = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_TRUE(fs::is_regular_file(state / "session.marker"));
  EXPECT_FALSE(owned_catalog.unpersisted());
  const auto finished = coordinator.finish_session();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(finished));
  EXPECT_EQ(std::get<storage::SaveOutcome>(finished), storage::SaveOutcome::SkippedPersisted);
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_EQ(fs::last_write_time(file), modified);
  EXPECT_FALSE(fs::exists(state / "session.marker"));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Stopped);
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.refresh_all(), storage::CoordinatorErrorCode::Lifecycle);
}

TEST_F(CatalogCoordinatorTest, PreviousMarkerOverridesValidStaleIndexAndIsNotReplaced) {
  seed(make_catalog());
  const auto cached = read_bytes(file);
  fs::create_directories(state);
  write_bytes(state / "session.marker", "previous marker need not have a payload schema");
  const auto time = fs::last_write_time(state / "session.marker");
  storage::NormalRepository repository{normal};
  const auto log = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  auto loaded = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  const auto& outcome = std::get<storage::InitializationOutcome>(loaded);
  EXPECT_TRUE(outcome.previous_session_incomplete);
  EXPECT_FALSE(outcome.index_error); // Cache was bypassed, not read and rejected.
  EXPECT_EQ(owned_catalog.read_view().query().size(), 1U);
  EXPECT_TRUE(owned_catalog.read_view().find_record(log.id));
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(file), cached);
  EXPECT_EQ(fs::last_write_time(state / "session.marker"), time);
  EXPECT_EQ(read_bytes(state / "session.marker"), "previous marker need not have a payload schema");
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  EXPECT_FALSE(fs::exists(state / "session.marker"));
}

TEST_F(CatalogCoordinatorTest,
       PreviousMarkerWithoutCacheRebuildsEmptyStorageWithoutSavingImmediately) {
  fs::create_directories(state);
  write_bytes(state / "session.marker", "");
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  auto loaded = coordinator.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_TRUE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(cache));
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  EXPECT_TRUE(fs::is_regular_file(file));
}

TEST_F(CatalogCoordinatorTest, PreviousMarkerRebuildFailurePreservesCatalogAndMarker) {
  seed(make_catalog());
  fs::create_directories(state);
  write_bytes(state / "session.marker", "retained");
  fs::create_directories(normal / "logs");
  write_bytes(normal / "logs/broken.md", "bad authoritative data");
  owned_catalog = make_catalog();
  const auto before = codec::encode_index(owned_catalog);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_EQ(std::get<std::string>(codec::encode_index(owned_catalog)),
            std::get<std::string>(before));
  expect_error(coordinator.finish_session(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(read_bytes(state / "session.marker"), "retained");
}

TEST_F(CatalogCoordinatorTest, UnsafeMarkerAndStateObjectsNeverBecomeReadyEvenThroughRefresh) {
  seed(make_catalog());
  fs::create_directories(state / "session.marker");
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_TRUE(owned_catalog.read_view().query().empty());
  expect_error(coordinator.refresh_all(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_NE(coordinator.status(), storage::CatalogStatus::Ready);
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::Lifecycle);
  fs::remove(state / "session.marker");
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(fs::is_regular_file(state / "session.marker"));
  EXPECT_TRUE(owned_catalog.unpersisted());
}

TEST_F(CatalogCoordinatorTest, MarkerCannotLiveInsideIndependentlyClearableCacheOrRuntime) {
  for (bool runtime : {false, true}) {
    auto invalid = paths();
    if (runtime)
      invalid.application.runtime = state.parent_path();
    else
      invalid.application.state = cache / "state";
    storage::MetadataCatalog catalog;
    storage::CatalogCoordinator coordinator{invalid, catalog};
    expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::InvalidPath);
    EXPECT_NE(coordinator.status(), storage::CatalogStatus::Ready);
  }
}

TEST_F(CatalogCoordinatorTest, AbandonedSessionForcesRebuildAfterAuthoritativeMutation) {
  seed(make_catalog());
  const auto stale = read_bytes(file);
  const auto log = authoritative_log();
  {
    storage::CatalogCoordinator abandoned{paths(), owned_catalog};
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(abandoned.initialize()));
    storage::NormalRepository repository{normal};
    ASSERT_TRUE(std::holds_alternative<std::monostate>(
        repository.create_log(log, abandoned.log_creation_callback(), abandoned.creation_check())));
    EXPECT_TRUE(owned_catalog.unpersisted());
  } // No destructor persistence or Marker cleanup.
  EXPECT_TRUE(fs::exists(state / "session.marker"));
  EXPECT_EQ(read_bytes(file), stale);
  storage::MetadataCatalog restored;
  storage::CatalogCoordinator next{paths(), restored};
  const auto loaded = next.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_TRUE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_TRUE(restored.read_view().find_record(log.id));
  EXPECT_EQ(restored.read_view().query().size(), 1U);
}

TEST_F(CatalogCoordinatorTest, InterruptedIndexWriteWithMarkerNeverPublishesPartialCatalog) {
  {
    storage::CatalogCoordinator abandoned{paths(), owned_catalog};
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(abandoned.initialize()));
  }
  fs::create_directories(file.parent_path());
  write_bytes(file, "incomplete index publication");
  storage::MetadataCatalog restored;
  storage::CatalogCoordinator next{paths(), restored};
  const auto loaded = next.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_TRUE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_TRUE(restored.read_view().query().empty());
  EXPECT_TRUE(restored.unpersisted());
  EXPECT_EQ(read_bytes(file), "incomplete index publication");
}

TEST_F(CatalogCoordinatorTest, SaveCheckpointRetainsMarkerUntilExplicitSessionFinish) {
  {
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
    ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.shutdown_save()));
    EXPECT_FALSE(owned_catalog.unpersisted());
    EXPECT_TRUE(fs::exists(state / "session.marker"));
  } // Crash-equivalent after durable Index, before Marker cleanup.
  storage::MetadataCatalog restored;
  storage::CatalogCoordinator next{paths(), restored};
  const auto loaded = next.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_TRUE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(next.finish_session()));
  storage::MetadataCatalog clean;
  storage::CatalogCoordinator final{paths(), clean};
  const auto clean_loaded = final.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(clean_loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(clean_loaded).previous_session_incomplete);
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(clean_loaded).index_error);
  EXPECT_FALSE(clean.unpersisted());
}

TEST_F(CatalogCoordinatorTest, SaveFailureAndInconsistencyCannotClearMarker) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  change_log_title(owned_catalog);
  fs::remove(file);
  fs::create_directory(file);
  expect_error(coordinator.finish_session(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(fs::exists(state / "session.marker"));
  const storage::RepositoryMutationError failure{
      storage::MutationFailureStage::AuthoritativeStateMayHaveChanged,
      folio::io::FileError{
          folio::io::FileErrorCode::IoError, normal, "original failure", {}, true}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      coordinator.log_update_callback()(std::cref(failure))));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  expect_error(coordinator.finish_session(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_TRUE(fs::exists(state / "session.marker"));
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(CatalogCoordinatorTest, MissingCleanCacheCannotCertifyCleanShutdown) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  fs::remove_all(state / "index");
  expect_error(coordinator.finish_session(), storage::CoordinatorErrorCode::Missing);
  EXPECT_TRUE(fs::exists(state / "session.marker"));
}

#ifndef _WIN32
struct LifecycleSyncGuard {
  folio::io::internal::SyncFile prior;
  explicit LifecycleSyncGuard(folio::io::internal::SyncFile hook)
      : prior(storage::internal::set_lifecycle_sync_for_testing(hook)) {}
  ~LifecycleSyncGuard() { storage::internal::set_lifecycle_sync_for_testing(prior); }
};
bool fail_lifecycle_sync(int) {
  errno = EIO;
  return false;
}

TEST_F(CatalogCoordinatorTest, FailedMarkerDurabilityCannotPublishDecodedOrRebuiltCatalog) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  {
    LifecycleSyncGuard fail{fail_lifecycle_sync};
    const auto loaded = coordinator.initialize();
    expect_error(loaded, storage::CoordinatorErrorCode::StorageFailure);
    ASSERT_TRUE(std::get<storage::CoordinatorError>(loaded).file_error);
    EXPECT_EQ(std::get<storage::CoordinatorError>(loaded).file_error->code,
              folio::io::FileErrorCode::DurabilityFailure);
    EXPECT_TRUE(owned_catalog.read_view().query().empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
    expect_error(coordinator.refresh_all(), storage::CoordinatorErrorCode::StorageFailure);
    EXPECT_TRUE(owned_catalog.read_view().query().empty());
  }
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_TRUE(fs::is_regular_file(state / "session.marker"));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(CatalogCoordinatorTest, ExistingMarkerSymlinkAndStateSymlinkAreRejected) {
  fs::create_directories(state);
  write_bytes(state / "target", "keep");
  fs::create_symlink(state / "target", state / "session.marker");
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(read_bytes(state / "target"), "keep");
  fs::remove(state / "session.marker");
  const auto actual = temporary.path() / "actual-state";
  fs::rename(state, actual);
  fs::create_directory_symlink(actual, state);
  storage::MetadataCatalog catalog;
  storage::CatalogCoordinator second{paths(), catalog};
  expect_error(second.initialize(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_FALSE(fs::exists(actual / "session.marker"));
}

TEST_F(CatalogCoordinatorTest, RuntimeCrudAndRefreshDoNotRewriteMarkerOrIndex) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  const auto marker_time = fs::last_write_time(state / "session.marker");
  const auto cached = read_bytes(file);
  {
    LifecycleSyncGuard reject_lifecycle_io{fail_lifecycle_sync};
    storage::NormalRepository repository{normal};
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(
        authoritative_log(), coordinator.log_creation_callback(), coordinator.creation_check())));
    EXPECT_TRUE(owned_catalog.read_view().find_record(authoritative_log().id));
    EXPECT_EQ(owned_catalog.read_view().query().size(), 6U);
    ASSERT_TRUE(
        std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  }
  EXPECT_EQ(fs::last_write_time(state / "session.marker"), marker_time);
  EXPECT_EQ(read_bytes(file), cached);
}

TEST_F(CatalogCoordinatorTest, FailureBeforeMarkerPublicationLeavesValidCacheUsableNextSession) {
  seed(make_catalog());
  fs::create_directories(state);
  fs::create_directories(normal / "logs");
  write_bytes(normal / "logs/broken.md", "proves next startup did not scan");
  {
    LifecycleSyncGuard fail_before_creation{fail_lifecycle_sync};
    storage::CatalogCoordinator blocked{paths(), owned_catalog};
    expect_error(blocked.initialize(), storage::CoordinatorErrorCode::StorageFailure);
    EXPECT_EQ(blocked.status(), storage::CatalogStatus::Unavailable);
    EXPECT_TRUE(owned_catalog.read_view().query().empty());
    EXPECT_FALSE(fs::exists(state / "session.marker"));
  }
  storage::CatalogCoordinator next{paths(), owned_catalog};
  const auto loaded = next.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_FALSE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_EQ(owned_catalog.read_view().query().size(), 5U);
}

bool fail_regular_file_sync(int fd) {
  struct stat status{};
  if (fstat(fd, &status) == 0 && S_ISREG(status.st_mode)) {
    errno = EIO;
    return false;
  }
  return fsync(fd) == 0;
}

TEST_F(CatalogCoordinatorTest, ExistingMarkerSyncFailureRetainsMarkerAndDoesNotAdoptCandidate) {
  fs::create_directories(state);
  write_bytes(state / "session.marker", "retained");
  owned_catalog = make_catalog();
  const auto before = std::get<std::string>(codec::encode_index(owned_catalog));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  LifecycleSyncGuard fail_marker_file{fail_regular_file_sync};
  expect_error(coordinator.initialize(), storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_EQ(read_bytes(state / "session.marker"), "retained");
  EXPECT_EQ(std::get<std::string>(codec::encode_index(owned_catalog)), before);
}

TEST_F(CatalogCoordinatorTest, MarkerCreationSyncFailureRetainsPublishedMarkerAndRecoveryCanRetry) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  {
    LifecycleSyncGuard fail_file_sync{fail_regular_file_sync};
    const auto loaded = coordinator.initialize();
    expect_error(loaded, storage::CoordinatorErrorCode::StorageFailure);
    ASSERT_TRUE(std::get<storage::CoordinatorError>(loaded).file_error);
    EXPECT_TRUE(std::get<storage::CoordinatorError>(loaded).file_error->replacement_completed);
    EXPECT_TRUE(fs::is_regular_file(state / "session.marker"));
    EXPECT_TRUE(owned_catalog.read_view().query().empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  }
  storage::MetadataCatalog recovered;
  storage::CatalogCoordinator next{paths(), recovered};
  const auto loaded = next.initialize();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(loaded));
  EXPECT_TRUE(std::get<storage::InitializationOutcome>(loaded).previous_session_incomplete);
  EXPECT_TRUE(recovered.unpersisted());
}

TEST_F(CatalogCoordinatorTest, FinalIndexSyncFailureCannotClearMarker) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  {
    LifecycleSyncGuard fail_final_sync{fail_regular_file_sync};
    expect_error(coordinator.finish_session(), storage::CoordinatorErrorCode::StorageFailure);
    EXPECT_TRUE(fs::is_regular_file(state / "session.marker"));
  }
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  EXPECT_FALSE(fs::exists(state / "session.marker"));
}

TEST_F(CatalogCoordinatorTest, DurableFilesystemHelpersRejectUnsafeTargetsAndAllowRemovalRetry) {
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      folio::io::create_directories_durable(state / "nested")));
  const auto target = state / "nested/file";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::create_empty_file_durable(target)));
  EXPECT_TRUE(
      std::holds_alternative<folio::io::FileError>(folio::io::create_empty_file_durable(target)));
  EXPECT_TRUE(read_bytes(target).empty());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::sync_regular_file(target)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::remove_file_durable(target)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::remove_file_durable(target)));
  EXPECT_TRUE(
      std::holds_alternative<folio::io::FileError>(folio::io::sync_regular_file(state / "nested")));
  EXPECT_TRUE(std::holds_alternative<folio::io::FileError>(
      folio::io::remove_file_durable(state / "nested")));
  fs::create_directory_symlink(state / "nested", state / "alias");
  EXPECT_TRUE(std::holds_alternative<folio::io::FileError>(
      folio::io::create_directories_durable(state / "alias/child")));
  EXPECT_FALSE(fs::exists(state / "nested/child"));
  write_bytes(target, "keep");
  fs::create_symlink(target, state / "link");
  EXPECT_TRUE(
      std::holds_alternative<folio::io::FileError>(folio::io::sync_regular_file(state / "link")));
  EXPECT_TRUE(
      std::holds_alternative<folio::io::FileError>(folio::io::remove_file_durable(state / "link")));
  EXPECT_TRUE(std::holds_alternative<folio::io::FileError>(
      folio::io::remove_file_durable(state / "alias/file")));
  EXPECT_TRUE(std::holds_alternative<folio::io::FileError>(
      folio::io::create_directories_durable("relative")));
  EXPECT_TRUE(
      std::holds_alternative<folio::io::FileError>(folio::io::remove_file_durable("relative")));
  EXPECT_EQ(read_bytes(target), "keep");
}

TEST_F(CatalogCoordinatorTest, ExistingDirectoryLinksAreSyncedOnRetry) {
  fs::create_directories(state / "nested");
  {
    LifecycleSyncGuard fail{fail_lifecycle_sync};
    const auto failed = folio::io::create_directories_durable(state / "nested");
    ASSERT_TRUE(std::holds_alternative<folio::io::FileError>(failed));
    EXPECT_EQ(std::get<folio::io::FileError>(failed).code,
              folio::io::FileErrorCode::DurabilityFailure);
  }
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      folio::io::create_directories_durable(state / "nested")));
}

fs::path cleanup_marker;
bool fail_marker_unlink_sync(int fd) {
  struct stat descriptor{}, directory{};
  if (fstat(fd, &descriptor) == 0 && stat(cleanup_marker.parent_path().c_str(), &directory) == 0 &&
      descriptor.st_dev == directory.st_dev && descriptor.st_ino == directory.st_ino &&
      !fs::exists(cleanup_marker)) {
    errno = EIO;
    return false;
  }
  return fsync(fd) == 0;
}

TEST_F(CatalogCoordinatorTest, MarkerRemovalSyncFailurePreservesOriginalErrorAndSupportsRetry) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  cleanup_marker = state / "session.marker";
  {
    LifecycleSyncGuard fail_cleanup{fail_marker_unlink_sync};
    const auto failed = coordinator.finish_session();
    expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
    ASSERT_TRUE(std::get<storage::CoordinatorError>(failed).file_error);
    const auto& cause = *std::get<storage::CoordinatorError>(failed).file_error;
    EXPECT_EQ(cause.code, folio::io::FileErrorCode::DurabilityFailure);
    EXPECT_TRUE(cause.replacement_completed); // Unlink occurred; durability uncertain.
    EXPECT_EQ(cause.system_error, std::error_code(EIO, std::generic_category()));
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
    EXPECT_FALSE(owned_catalog.unpersisted()); // Index really did save successfully.
  }
  const auto bytes = read_bytes(file);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  EXPECT_EQ(read_bytes(file), bytes);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Stopped);
  EXPECT_FALSE(fs::exists(state / "session.marker"));
}
#endif

} // namespace

namespace {
TEST_F(CatalogCoordinatorTest, NotificationFailureCannotChangeAuthoritativeOrCatalogSuccess) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.initialize()));
  storage::NormalRepository repository{normal};
  const auto old_view = coordinator.read_state().view;
  unsigned failures = 0;
  coordinator.set_change_observer([](const auto&) { throw std::runtime_error("delivery failed"); },
                              [&] { ++failures; });
  const auto record = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(
      record, coordinator.log_creation_callback(), coordinator.creation_check())));
  EXPECT_EQ(failures, 1u);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(coordinator.read_state().view.find_record(record.id));
  EXPECT_FALSE(old_view.find_record(record.id));
  EXPECT_TRUE(std::holds_alternative<core::Log::Snapshot>(repository.load_log(record.id)));
  ASSERT_TRUE(
      std::holds_alternative<std::vector<storage::ExcludedObject>>(coordinator.refresh_all()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(failures, 2u);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.publish_scoped_refresh(
      owned_catalog, [] { throw std::runtime_error("scoped delivery failed"); })));
  EXPECT_EQ(failures, 3u);
}
} // namespace
