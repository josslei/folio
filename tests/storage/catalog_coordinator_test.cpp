#include "storage/catalog_coordinator.hpp"
#include "storage/normal_repository.hpp"

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
void expect_error(const ::folio::core::internal::Result<T, storage::CoordinatorError>& result,
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
  for (const storage::RecordMetadata& entry : {
           storage::RecordMetadata{log}, storage::RecordMetadata{memo},
           storage::RecordMetadata{todo}, storage::RecordMetadata{root_todo},
           storage::RecordMetadata{timed_todo}}) {
    if (!std::holds_alternative<std::monostate>(catalog.insert_record(entry)))
      throw std::runtime_error("Invalid Entry fixture");
  }
  return catalog;
}

core::Log::Snapshot authoritative_log() {
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", std::nullopt};
  return {id("a7183823-2574-4bfd-b411-99ed177d3e43"), created, created,
          "Authoritative Log", "synthetic body",
          {{std::chrono::year{2027} / 1 / 15}, "UTC", std::nullopt},
          {{"rebuild"}}, core::Privacy::Normal};
}

core::Memo::Snapshot authoritative_memo(const core::Id& directory_id) {
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
          std::chrono::seconds{1800000000} + std::chrono::nanoseconds{123456000})},
      "Asia/Shanghai", "上海"};
  return {id("b7183823-2574-4bfd-b411-99ed177d3e43"), created, created,
          "研究 Memo", "synthetic content", {{"research"}}, directory_id,
          core::Privacy::Normal};
}

core::Todo::Snapshot authoritative_todo(const core::Id& directory_id) {
  const auto created = core::TimestampContext{
      core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", std::nullopt};
  return {id("c7183823-2574-4bfd-b411-99ed177d3e43"), created, created,
          "旅行 Todo", "synthetic notes", core::TodoStatus::Pending,
          core::TodoPriority::High,
          core::DueDate{core::DateContext{{std::chrono::year{2028} / 3 / 4},
                                          "Asia/Tokyo", "東京"}},
          directory_id, core::Privacy::Normal, {{"Book train", false}}};
}

void change_log_title(storage::MetadataCatalog& catalog) {
  const auto found = catalog.find_record(
      id("17183823-2574-4bfd-b411-99ed177d3e43"));
  if (!found) throw std::runtime_error("Missing test Log");
  auto log = std::get<storage::LogMetadata>(*found);
  log.title = "Changed after first save";
  if (!std::holds_alternative<std::monostate>(catalog.update_record(
          storage::RecordMetadata{log})))
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

TEST_F(CatalogCoordinatorTest, BorrowedCatalogSurvivesCoordinatorWithoutConstructorIo) {
  owned_catalog = make_catalog();
  const auto before = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(before));
  {
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
    EXPECT_FALSE(fs::exists(normal));
    EXPECT_FALSE(fs::exists(vault));
    EXPECT_FALSE(fs::exists(state));
    EXPECT_FALSE(fs::exists(cache));
    EXPECT_EQ(owned_catalog.query().size(), 5U);
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
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  auto catalog = make_catalog();
  owned_catalog = std::move(catalog);
  EXPECT_TRUE(owned_catalog.unpersisted());
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(read_bytes(file), "corrupt cache");
  EXPECT_EQ(read_bytes(normal / "logs" / "malformed.md"), "bad Entry");
  EXPECT_FALSE(fs::exists(vault));
  EXPECT_FALSE(fs::exists(cache));
}

TEST_F(CatalogCoordinatorTest, ShutdownBeforeStartupDoesNotBlockStartup) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unloaded);
  seed(make_catalog());
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::SkippedPersisted);
}

TEST_F(CatalogCoordinatorTest, PersistedLoadedCatalogSkipsShutdownIoAndQueriesMemory) {
  const auto original = make_catalog();
  seed(original);
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.query().size(), 5U);
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Log).records.size(), 1U);
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Memo).directories.size(), 1U);
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Todo).directories.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo,
          id("87183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Todo,
          id("97183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  const auto encoded = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(std::get<std::string>(encoded), read_bytes(file));
  ASSERT_TRUE(fs::remove(file));
  EXPECT_EQ(owned_catalog.query().size(), 5U);
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
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  for (const auto& entry : owned_catalog.query()) {
    EXPECT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.remove_record(
        std::visit([](const auto& value) { return value.id; }, entry))));
  }
  // Directory records must be removed after their members.
  for (const auto kind : {core::DirectoryKind::Memo, core::DirectoryKind::Todo}) {
    for (const auto& directory : owned_catalog.list_directories(kind))
      EXPECT_TRUE(std::holds_alternative<std::monostate>(
          owned_catalog.remove_directory(directory.id)));
  }
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.query().empty());
  ASSERT_TRUE(fs::remove(file));
  const auto result = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(result));
  EXPECT_EQ(std::get<storage::SaveOutcome>(result), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(owned_catalog.unpersisted());
  const auto decoded = codec::decode_index(read_bytes(file));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  EXPECT_TRUE(std::get<storage::MetadataCatalog>(decoded).query().empty());
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, StartupCacheErrorsReportFallbackReasonsAndCannotRetry) {
  auto check = [&](storage::CoordinatorErrorCode expected) {
    const auto before = fs::exists(file) ? read_bytes(file) : std::string{};
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    const auto started = coordinator.startup_load();
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
    expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  };
  check(storage::CoordinatorErrorCode::Missing);
  EXPECT_FALSE(fs::exists(state));
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
  packer.pack(std::string{"directories"}); packer.pack_array(0);
  packer.pack(std::string{"entries"}); packer.pack_array(0);
  packer.pack(std::string{"schema_version"}); packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  check(storage::CoordinatorErrorCode::UnsupportedVersion);
  expect_no_data_roots();
}

TEST_F(CatalogCoordinatorTest, InvalidRootsAndIoFailureDoNotCreateCache) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::MetadataCatalog overlap_catalog;
  storage::CatalogCoordinator overlap{invalid, overlap_catalog};
  expect_error(overlap.startup_load(), storage::CoordinatorErrorCode::InvalidPath);
  expect_error(overlap.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
  invalid = paths();
  invalid.state_root = temporary.path() / std::string(8192, 'x');
  storage::MetadataCatalog io_catalog;
  storage::CatalogCoordinator io{invalid, io_catalog};
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
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  auto log = std::get<storage::LogMetadata>(*owned_catalog.find_record(
      id("17183823-2574-4bfd-b411-99ed177d3e43")));
  log.log_date.date = std::chrono::year{-1} / 1 / 1;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.update_record(
      storage::RecordMetadata{log})));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::CodecError);
  ASSERT_TRUE(std::get<storage::CoordinatorError>(failed).codec_error.has_value());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(owned_catalog.unpersisted());
  log.log_date.date = std::chrono::year{2027} / 1 / 15;
  log.title = "Recovered after failed encoding";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(owned_catalog.update_record(
      storage::RecordMetadata{log})));
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  change_log_title(owned_catalog);
  const auto pending = file.parent_path() /
      ("." + file.filename().string() + ".tmp-" + core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished cache"));
  const auto failed = coordinator.shutdown_save();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  const auto& cause = std::get<storage::CoordinatorError>(failed);
  ASSERT_TRUE(cause.storage_error.has_value());
  EXPECT_EQ(cause.storage_error->code, storage::StorageErrorCode::RecoveryRequired);
  EXPECT_FALSE(cause.storage_error->replacement_completed);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_EQ(read_bytes(pending), "unfinished cache");
  EXPECT_EQ(std::distance(fs::directory_iterator(file.parent_path()),
                          fs::directory_iterator{}), 2);
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(parent_file.startup_load()));
  change_log_title(parent_catalog);
  ASSERT_TRUE(fs::remove(file));
  ASSERT_TRUE(fs::remove(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file.parent_path(), "not a directory"));
  const auto blocked = parent_file.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(blocked));
  EXPECT_EQ(read_bytes(file.parent_path()), "not a directory");
  EXPECT_TRUE(parent_catalog.unpersisted());
  ASSERT_TRUE(fs::remove(file.parent_path()));

  seed(make_catalog());
  storage::MetadataCatalog symlink_catalog;
  storage::CatalogCoordinator symlinked{paths(), symlink_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(symlinked.startup_load()));
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
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
  const auto started = coordinator.startup_load();
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
  const auto found = owned_catalog.find_record(snapshot.id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(std::get<storage::LogMetadata>(*found).title, snapshot.title);
  EXPECT_EQ(std::get<storage::LogMetadata>(*found).created.timestamp,
            snapshot.created.timestamp);
  EXPECT_FALSE(fs::exists(state));
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(vault));
  EXPECT_FALSE(fs::exists(cache));
}

TEST_F(CatalogCoordinatorTest, CorruptIndexCanRebuildAndReplacesItOnlyOnSave) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "corrupt index"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto started = coordinator.startup_load();
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
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
  EXPECT_TRUE(owned_catalog.query().empty());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(state));
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
  const auto& diagnostics =
      std::get<std::vector<storage::ExcludedObject>>(rebuilt);
  ASSERT_EQ(diagnostics.size(), 1U);
  EXPECT_FALSE(diagnostics.front().path.empty());
  EXPECT_EQ(owned_catalog.query().size(), 1U);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(state));
}

TEST_F(CatalogCoordinatorTest, RebuildBeforeStartupPreventsCacheOverwriteByStartupLoad) {
  seed(make_catalog());
  const auto cache_bytes = read_bytes(file);
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(
      coordinator.refresh_all()));
  EXPECT_TRUE(owned_catalog.find_record(snapshot.id).has_value());
  EXPECT_FALSE(owned_catalog.find_record(
      id("17183823-2574-4bfd-b411-99ed177d3e43")).has_value());
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
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
  const auto initialized = coordinator.startup_load();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  EXPECT_FALSE(outcome.index_error.has_value());
  EXPECT_TRUE(outcome.diagnostics.empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.query().size(), 5U);
  EXPECT_EQ(read_bytes(file), old_bytes);
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
}

TEST_F(CatalogCoordinatorTest, StartupFallsBackForMissingCorruptAndUnsupportedIndex) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  auto verify_fallback = [&](storage::CoordinatorErrorCode expected) {
    const auto previous = fs::exists(file) ? read_bytes(file) : std::string{};
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    const auto result = coordinator.startup_load();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(result));
    const auto& outcome = std::get<storage::InitializationOutcome>(result);
    ASSERT_TRUE(outcome.index_error.has_value());
    EXPECT_EQ(outcome.index_error->code, expected);
    EXPECT_TRUE(outcome.diagnostics.empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    EXPECT_TRUE(owned_catalog.unpersisted());
    EXPECT_TRUE(owned_catalog.find_record(snapshot.id).has_value());
    EXPECT_EQ(fs::exists(file) ? read_bytes(file) : std::string{}, previous);
    expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  };
  verify_fallback(storage::CoordinatorErrorCode::Missing);
  EXPECT_FALSE(fs::exists(state));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "bad MessagePack"));
  verify_fallback(storage::CoordinatorErrorCode::Corrupt);
  msgpack::sbuffer packed;
  msgpack::packer<msgpack::sbuffer> packer{packed};
  packer.pack_map(3);
  packer.pack(std::string{"directories"}); packer.pack_array(0);
  packer.pack(std::string{"entries"}); packer.pack_array(0);
  packer.pack(std::string{"schema_version"}); packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  verify_fallback(storage::CoordinatorErrorCode::UnsupportedVersion);
}

TEST_F(CatalogCoordinatorTest, StartupFallbackRestoresMixedMetadataAndDirectoryMembership) {
  storage::NormalRepository repository{normal};
  const core::Directory::Snapshot memo_directory{
      id("d7183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Memo,
      "研究 / Café"};
  const core::Directory::Snapshot todo_directory{
      id("e7183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Todo,
      "旅行"};
  const auto log = authoritative_log();
  const auto memo = authoritative_memo(memo_directory.id);
  const auto todo = authoritative_todo(todo_directory.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Memo, memo_directory)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      core::DirectoryKind::Todo, todo_directory)));
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
      storage::metadata_from(memo_directory,
                             fs::path{"memos"} / memo_directory.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_directory(
      storage::metadata_from(todo_directory,
                             fs::path{"todos"} / todo_directory.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_record(
      storage::metadata_from(log, std::get<fs::path>(log_path)))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_record(
      storage::metadata_from(memo, renamed))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_record(
      storage::metadata_from(todo, std::get<fs::path>(todo_path)))));
  const auto expected_bytes = codec::encode_index(expected);
  ASSERT_TRUE(std::holds_alternative<std::string>(expected_bytes));

  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.startup_load();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  ASSERT_TRUE(std::get<storage::InitializationOutcome>(initialized).index_error.has_value());
  EXPECT_EQ(std::get<storage::InitializationOutcome>(initialized).index_error->code,
            storage::CoordinatorErrorCode::Missing);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto actual_bytes = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(actual_bytes));
  EXPECT_EQ(std::get<std::string>(actual_bytes), std::get<std::string>(expected_bytes));
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Log).records.size(), 1U);
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Memo).records.size(), 0U);
  EXPECT_EQ(owned_catalog.list_root(storage::RecordType::Todo).records.size(), 0U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Memo,
                                           memo_directory.id)).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::RecordMetadata>>(
      owned_catalog.list_directory(core::DirectoryKind::Todo,
                                           todo_directory.id)).size(), 1U);
  EXPECT_FALSE(fs::exists(state));
}

TEST_F(CatalogCoordinatorTest, StartupReturnsExclusionsFromFallbackAndSavesOnlyLater) {
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.txt", "ignored"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.startup_load();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  ASSERT_TRUE(outcome.index_error.has_value());
  EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
  ASSERT_EQ(outcome.diagnostics.size(), 1U);
  EXPECT_FALSE(outcome.diagnostics.front().path.empty());
  EXPECT_FALSE(fs::exists(state));
  const auto saved = coordinator.shutdown_save();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_TRUE(fs::is_regular_file(file));
}

TEST_F(CatalogCoordinatorTest, StartupMissingIndexAndEmptyNormalRootBuildsEmptyReadyCatalog) {
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  const auto initialized = coordinator.startup_load();
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(initialized));
  const auto& outcome = std::get<storage::InitializationOutcome>(initialized);
  ASSERT_TRUE(outcome.index_error.has_value());
  EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::Missing);
  EXPECT_TRUE(outcome.diagnostics.empty());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.query().empty());
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(state));
}

TEST_F(CatalogCoordinatorTest, StartupPropagatesPathAndIoErrorsWithoutScanning) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::MetadataCatalog overlap_catalog;
  storage::CatalogCoordinator overlap{invalid, overlap_catalog};
  expect_error(overlap.startup_load(), storage::CoordinatorErrorCode::InvalidPath);
  EXPECT_EQ(overlap.status(), storage::CatalogStatus::Unavailable);
  EXPECT_FALSE(fs::exists(normal));
  invalid = paths();
  invalid.state_root = temporary.path() / std::string(8192, 'x');
  storage::MetadataCatalog io_catalog;
  storage::CatalogCoordinator io{invalid, io_catalog};
  expect_error(io.startup_load(), storage::CoordinatorErrorCode::IoError);
  EXPECT_EQ(io.status(), storage::CatalogStatus::Unavailable);
  EXPECT_FALSE(fs::exists(normal));
  EXPECT_FALSE(fs::exists(state));
}

TEST_F(CatalogCoordinatorTest, FailedFallbackScanPreservesCatalogAndUnavailableStatus) {
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad front matter"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  owned_catalog = make_catalog();
  const auto before = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(before));
  const auto failed = coordinator.startup_load();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Unavailable);
  EXPECT_TRUE(owned_catalog.unpersisted());
  const auto after = codec::encode_index(owned_catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(after));
  EXPECT_EQ(std::get<std::string>(after), std::get<std::string>(before));
  EXPECT_FALSE(fs::exists(state));
  expect_error(coordinator.startup_load(), storage::CoordinatorErrorCode::Lifecycle);
  expect_error(coordinator.shutdown_save(), storage::CoordinatorErrorCode::Unavailable);
}

TEST_F(CatalogCoordinatorTest, FailedRefreshPreservesPersistedCatalogAndReadyStatus) {
  seed(make_catalog());
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  ASSERT_FALSE(owned_catalog.unpersisted());
  const auto original = read_bytes(file);
  ASSERT_TRUE(fs::create_directories(normal / "logs"));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "malformed.md", "bad front matter"));
  const auto failed = coordinator.refresh_all();
  expect_error(failed, storage::CoordinatorErrorCode::StorageFailure);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_FALSE(owned_catalog.unpersisted());
  EXPECT_EQ(owned_catalog.query().size(), 5U);
  EXPECT_EQ(read_bytes(file), original);
}

TEST_F(CatalogCoordinatorTest, ExplicitRefreshReplacesCatalogWithoutIndexIo) {
  seed(make_catalog());
  storage::NormalRepository repository{normal};
  const auto snapshot = authoritative_log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(snapshot)));
  ASSERT_NO_FATAL_FAILURE(write_bytes(normal / "logs" / "unrelated.txt", "ignored"));
  storage::CatalogCoordinator coordinator{paths(), owned_catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "corrupt after initialization"));
  const auto cached_bytes = read_bytes(file);
  const auto refreshed = coordinator.refresh_all();
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::ExcludedObject>>(refreshed));
  EXPECT_EQ(std::get<std::vector<storage::ExcludedObject>>(refreshed).size(), 1U);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(owned_catalog.unpersisted());
  EXPECT_TRUE(owned_catalog.find_record(snapshot.id).has_value());
  EXPECT_FALSE(owned_catalog.find_record(
      id("17183823-2574-4bfd-b411-99ed177d3e43")).has_value());
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
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  change_log_title(owned_catalog);
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
  EXPECT_TRUE(owned_catalog.unpersisted());
  expect_no_data_roots();
}
#endif

}  // namespace
