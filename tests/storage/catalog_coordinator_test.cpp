#include "storage/catalog_coordinator.hpp"

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
void expect_error(const storage::CoordinatorResult<T>& result,
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
      !std::holds_alternative<std::monostate>(catalog.insert_directory(
          {todo_dir, core::DirectoryKind::Todo, "todos/group", "任务"})))
    throw std::runtime_error("Invalid Directory fixture");
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000000} + std::chrono::nanoseconds{123456000})},
      "Asia/Shanghai", "上海"};
  const auto modified = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000010} + std::chrono::nanoseconds{987654000})},
      "Europe/Paris", std::nullopt};
  const storage::LogMetadata log{
      id("17183823-2574-4bfd-b411-99ed177d3e43"), "logs/one.md",
      core::Privacy::Protected, created, modified, "Log 旅行", {{"daily"}, {"旅行"}},
      {{std::chrono::year{2027} / 1 / 15}, "Asia/Tokyo", "東京"}};
  const storage::MemoMetadata memo{
      id("27183823-2574-4bfd-b411-99ed177d3e43"), "memos/group/one.md",
      core::Privacy::Normal, created, modified, "Memo Café", {{"research"}}, memo_dir};
  const storage::TodoMetadata todo{
      id("37183823-2574-4bfd-b411-99ed177d3e43"), "todos/group/one.toml",
      core::Privacy::Hidden, created, modified, "Todo 日付",
      core::TodoStatus::Completed, core::TodoPriority::High,
      core::DueDate{core::DateContext{{std::chrono::year{2028} / 2 / 3},
                                      "Asia/Tokyo", "東京"}}, todo_dir};
  const storage::TodoMetadata root_todo{
      id("47183823-2574-4bfd-b411-99ed177d3e43"), "todos/root.toml",
      core::Privacy::Normal, created, modified, "Root Todo",
      core::TodoStatus::Pending, core::TodoPriority::None,
      std::nullopt, std::nullopt};
  const storage::TodoMetadata timed_todo{
      id("58183823-2574-4bfd-b411-99ed177d3e43"), "todos/timed.toml",
      core::Privacy::Normal, created, modified, "Timed Todo",
      core::TodoStatus::Pending, core::TodoPriority::Medium,
      core::DueDate{core::TimestampContext{
          core::Timestamp{std::chrono::seconds{1900000000}}, "UTC", std::nullopt}},
      std::nullopt};
  for (const storage::EntryMetadata& entry : {
           storage::EntryMetadata{log}, storage::EntryMetadata{memo},
           storage::EntryMetadata{todo}, storage::EntryMetadata{root_todo},
           storage::EntryMetadata{timed_todo}}) {
    if (!std::holds_alternative<std::monostate>(catalog.insert_entry(entry)))
      throw std::runtime_error("Invalid Entry fixture");
  }
  return catalog;
}

void change_log_title(storage::MetadataCatalog& catalog) {
  const auto found = catalog.find_entry(
      id("17183823-2574-4bfd-b411-99ed177d3e43"));
  if (!found) throw std::runtime_error("Missing test Log");
  auto log = std::get<storage::LogMetadata>(*found);
  log.title = "Changed after first save";
  if (!std::holds_alternative<std::monostate>(catalog.update_entry(
          storage::EntryMetadata{log})))
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

  storage::StoragePaths paths() const { return {normal, vault, state, cache}; }
  void seed(const storage::MetadataCatalog& catalog) const {
    const auto encoded = codec::encode_index(catalog);
    ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
    ASSERT_TRUE(fs::create_directories(file.parent_path()) ||
                fs::is_directory(file.parent_path()));
    ASSERT_NO_FATAL_FAILURE(write_bytes(file, std::get<std::string>(encoded)));
  }
  void expect_no_data_roots() const {
    EXPECT_FALSE(fs::exists(normal));
    EXPECT_FALSE(fs::exists(vault));
    EXPECT_FALSE(fs::exists(cache));
  }
};

TEST_F(CatalogCoordinatorTest, ConstructorAndFailedStartupNeverOverwriteCache) {
  storage::CatalogCoordinator coordinator{paths()};
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
  EXPECT_FALSE(fs::exists(state));
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Missing);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_FALSE(fs::exists(state));
  auto catalog = make_catalog();
  coordinator.catalog() = std::move(catalog);
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_FALSE(fs::exists(state));
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, ShutdownBeforeStartupDoesNotBlockStartup) {
  storage::CatalogCoordinator coordinator{paths()};
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
  seed(make_catalog());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::SkippedPersisted);
}

TEST_F(CatalogCoordinatorTest, PersistedLoadedCatalogSkipsShutdownIoAndQueriesMemory) {
  const auto original = make_catalog();
  seed(original);
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(coordinator.catalog().unpersisted());
  EXPECT_EQ(coordinator.catalog().query().size(), 5U);
  EXPECT_EQ(coordinator.catalog().list_root(storage::EntryType::Log).entries.size(), 1U);
  EXPECT_EQ(coordinator.catalog().list_root(storage::EntryType::Memo).directories.size(), 1U);
  EXPECT_EQ(coordinator.catalog().list_root(storage::EntryType::Todo).directories.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      coordinator.catalog().list_directory(core::DirectoryKind::Memo,
          id("87183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      coordinator.catalog().list_directory(core::DirectoryKind::Todo,
          id("97183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  const auto encoded = codec::encode_index(coordinator.catalog());
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(std::get<std::string>(encoded), read_bytes(file));
  ASSERT_TRUE(fs::remove(file));
  EXPECT_EQ(coordinator.catalog().query().size(), 5U);
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::SkippedPersisted);
  EXPECT_FALSE(fs::exists(file));
  const auto again = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(again));
  EXPECT_EQ(std::get<storage::SaveOutcome>(again), storage::SaveOutcome::SkippedPersisted);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, UnpersistedLoadedCatalogReplacesCacheAndAcknowledgesSave) {
  seed(make_catalog());
  const auto before = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  change_log_title(coordinator.catalog());
  ASSERT_TRUE(coordinator.catalog().unpersisted());
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::Saved);
  EXPECT_FALSE(coordinator.catalog().unpersisted());
  EXPECT_NE(read_bytes(file), before);
  const auto encoded = codec::encode_index(coordinator.catalog());
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
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  for (const auto& entry : coordinator.catalog().query()) {
    EXPECT_TRUE(std::holds_alternative<std::monostate>(coordinator.catalog().remove_entry(
        std::visit([](const auto& value) { return value.id; }, entry))));
  }
  // Directory records must be removed after their members.
  for (const auto kind : {core::DirectoryKind::Memo, core::DirectoryKind::Todo}) {
    for (const auto& directory : coordinator.catalog().list_directories(kind))
      EXPECT_TRUE(std::holds_alternative<std::monostate>(
          coordinator.catalog().remove_directory(directory.id)));
  }
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  EXPECT_TRUE(coordinator.catalog().query().empty());
  ASSERT_TRUE(fs::remove(file));
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(coordinator.catalog().unpersisted());
  const auto decoded = codec::decode_index(read_bytes(file));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  EXPECT_TRUE(std::get<storage::MetadataCatalog>(decoded).query().empty());
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, StartupErrorsAreDistinctAndCannotBeRetried) {
  storage::CatalogCoordinator missing{paths()};
  expect_error(missing.startup_load(), storage::CoordinatorErrorCode::Missing);
  expect_error(missing.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  EXPECT_FALSE(fs::exists(state));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "bad MessagePack"));
  storage::CatalogCoordinator corrupt{paths()};
  const auto bad = corrupt.startup_load();
  expect_error(bad, storage::CoordinatorErrorCode::Corrupt);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(bad).codec_error.has_value());
  const auto valid = codec::encode_index(make_catalog());
  ASSERT_TRUE(std::holds_alternative<std::string>(valid));
  auto truncated_bytes = std::get<std::string>(valid);
  truncated_bytes.resize(truncated_bytes.size() / 2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, truncated_bytes));
  storage::CatalogCoordinator truncated{paths()};
  expect_error(truncated.startup_load(), storage::CoordinatorErrorCode::Corrupt);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "changed after failed load"));
  expect_error(corrupt.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(corrupt.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(read_bytes(file), "changed after failed load");

  msgpack::sbuffer packed;
  msgpack::packer<msgpack::sbuffer> packer{packed};
  packer.pack_map(3);
  packer.pack(std::string{"directories"}); packer.pack_array(0);
  packer.pack(std::string{"entries"}); packer.pack_array(0);
  packer.pack(std::string{"schema_version"}); packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  storage::CatalogCoordinator unsupported{paths()};
  expect_error(unsupported.startup_load(), storage::CoordinatorErrorCode::UnsupportedVersion);
  const std::string oversized(64u * 1024u * 1024u + 1, '\0');
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, oversized));
  storage::CatalogCoordinator limit{paths()};
  expect_error(limit.startup_load(), storage::CoordinatorErrorCode::LimitExceeded);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, InvalidRootsAndIoFailureDoNotCreateCache) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::CatalogCoordinator overlap{invalid};
  expect_error(overlap.startup_load(), storage::CoordinatorErrorCode::InvalidPath);
  expect_error(overlap.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  invalid = paths();
  invalid.state_root = temporary.path() / std::string(8192, 'x');
  storage::CatalogCoordinator io{invalid};
  const auto failure = io.startup_load();
  expect_error(failure, storage::CoordinatorErrorCode::IoError);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failure).storage_error.has_value());
  EXPECT_TRUE(std::get<storage::CoordinatorError>(failure).storage_error->system_error);
  EXPECT_FALSE(fs::exists(state));
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, EncodingFailurePreservesExistingCacheAndUnpersistedCatalog) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  auto log = std::get<storage::LogMetadata>(*coordinator.catalog().find_entry(
      id("17183823-2574-4bfd-b411-99ed177d3e43")));
  log.log_date.date = std::chrono::year{-1} / 1 / 1;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.catalog().update_entry(
      storage::EntryMetadata{log})));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::CodecError);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failed).codec_error.has_value());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  log.log_date.date = std::chrono::year{2027} / 1 / 15;
  log.title = "Recovered after failed encoding";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.catalog().update_entry(
      storage::EntryMetadata{log})));
  const auto retried = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(retried));
  EXPECT_EQ(std::get<storage::SaveOutcome>(retried), storage::SaveOutcome::Saved);
  EXPECT_FALSE(coordinator.catalog().unpersisted());
  EXPECT_NE(read_bytes(file), original);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, PendingTemporaryBlocksReplacementWithoutNewTemp) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  change_log_title(coordinator.catalog());
  const auto pending = file.parent_path() /
      ("." + file.filename().string() + ".tmp-" + core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished cache"));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  const auto& cause = std::get<storage::CoordinatorError>(failed);
  ASSERT_TRUE(cause.storage_error.has_value());
  EXPECT_EQ(cause.storage_error->code, storage::StorageErrorCode::RecoveryRequired);
  EXPECT_FALSE(cause.storage_error->replacement_completed);
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_EQ(read_bytes(pending), "unfinished cache");
  EXPECT_EQ(std::distance(fs::directory_iterator(file.parent_path()),
                          fs::directory_iterator{}), 2);
  ASSERT_TRUE(fs::remove(pending));
  const auto retried = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(retried));
  EXPECT_EQ(std::get<storage::SaveOutcome>(retried), storage::SaveOutcome::Saved);
  EXPECT_FALSE(coordinator.catalog().unpersisted());
  EXPECT_NE(read_bytes(file), original);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, ParentFileAndSymlinksPreventShutdownWrite) {
  seed(make_catalog());
  storage::CatalogCoordinator parent_file{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(parent_file.startup_load()));
  change_log_title(parent_file.catalog());
  ASSERT_TRUE(fs::remove(file));
  ASSERT_TRUE(fs::remove(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file.parent_path(), "not a directory"));
  const auto blocked = parent_file.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(read_bytes(file.parent_path()), "not a directory");
  EXPECT_TRUE(parent_file.catalog().unpersisted());
  ASSERT_TRUE(fs::remove(file.parent_path()));

  seed(make_catalog());
  storage::CatalogCoordinator symlinked{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(symlinked.startup_load()));
  change_log_title(symlinked.catalog());
  const auto outside = temporary.path() / "outside.msgpack";
  ASSERT_NO_FATAL_FAILURE(write_bytes(outside, "outside bytes"));
  ASSERT_TRUE(fs::remove(file));
  std::error_code ec;
  fs::create_symlink(outside, file, ec);
  if (ec) GTEST_SKIP() << "Cannot create file symlink: " << ec.message();
  expect_error(symlinked.shutdown_save(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(read_bytes(outside), "outside bytes");
  EXPECT_TRUE(symlinked.catalog().unpersisted());
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, SymlinkedIndexParentCannotRedirectShutdownWrite) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  change_log_title(coordinator.catalog());
  ASSERT_TRUE(fs::remove(file));
  ASSERT_TRUE(fs::remove(file.parent_path()));
  const auto outside = temporary.path() / "outside";
  ASSERT_TRUE(fs::create_directory(outside));
  std::error_code ec;
  fs::create_directory_symlink(outside, file.parent_path(), ec);
  if (ec) GTEST_SKIP() << "Cannot create directory symlink: " << ec.message();
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_TRUE(fs::is_empty(outside));
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  expect_no_data_roots();
}

#ifndef _WIN32
TEST_F(CatalogCoordinatorTest, DirectoryLockFailurePreservesExistingCacheAndUnpersistedFlag) {
  seed(make_catalog());
  const auto original = read_bytes(file);
  storage::CatalogCoordinator coordinator{paths()};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(coordinator.startup_load()));
  change_log_title(coordinator.catalog());
  const int descriptor = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) GTEST_SKIP() << "Cannot open cache directory for locking";
  struct LockGuard {
    int fd;
    ~LockGuard() { (void)flock(fd, LOCK_UN); (void)close(fd); }
  } guard{descriptor};
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    GTEST_SKIP() << "Cannot acquire cache directory lock";
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::IoError);
  const auto& cause = std::get<storage::CoordinatorError>(failed);
  ASSERT_TRUE(cause.storage_error.has_value());
  EXPECT_EQ(cause.storage_error->code, storage::StorageErrorCode::IoError);
  EXPECT_FALSE(cause.storage_error->replacement_completed);
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(coordinator.catalog().unpersisted());
  expect_no_data_roots();
}
#endif

}  // namespace
