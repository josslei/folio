#include "storage/index_store.hpp"

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
void expect_error(const storage::IndexStoreResult<T>& result,
                  storage::IndexStoreErrorCode code) {
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(result));
  EXPECT_EQ(std::get<storage::IndexStoreError>(result).code, code);
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

storage::MetadataCatalog make_dirty_empty() {
  storage::MetadataCatalog catalog;
  const storage::MemoMetadata memo{
      id("57183823-2574-4bfd-b411-99ed177d3e43"), "temporary.md",
      core::Privacy::Normal,
      {core::Timestamp{std::chrono::seconds{1}}, "UTC", std::nullopt},
      {core::Timestamp{std::chrono::seconds{1}}, "UTC", std::nullopt},
      "Temporary", {}, std::nullopt};
  if (!std::holds_alternative<std::monostate>(catalog.insert_entry(
          storage::EntryMetadata{memo})) ||
      !std::holds_alternative<std::monostate>(catalog.remove_entry(memo.id)))
    throw std::runtime_error("Could not make dirty empty catalog");
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

class IndexStoreTest : public testing::Test {
 protected:
  TemporaryDirectory temporary;
  fs::path normal = temporary.path() / "normal";
  fs::path vault = temporary.path() / "vault";
  fs::path state = temporary.path() / "custom-state";
  fs::path cache = temporary.path() / "cache";
  fs::path file = state / "index" / "normal.msgpack";

  storage::StoragePaths paths() const { return {normal, vault, state, cache}; }
  void expect_no_data_roots() const {
    EXPECT_FALSE(fs::exists(normal));
    EXPECT_FALSE(fs::exists(vault));
    EXPECT_FALSE(fs::exists(cache));
  }
};

TEST_F(IndexStoreTest, CleanSaveSkipsIoButDirtyEmptyCatalogPersists) {
  storage::NormalIndexStore store{paths()};
  storage::MetadataCatalog clean;
  EXPECT_FALSE(clean.dirty());
  const auto skipped = store.save(clean);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(skipped));
  EXPECT_EQ(std::get<storage::SaveOutcome>(skipped),
            storage::SaveOutcome::SkippedClean);
  EXPECT_FALSE(fs::exists(state));
  expect_no_data_roots();

  auto empty = make_dirty_empty();
  EXPECT_TRUE(empty.dirty());
  const auto saved = store.save(empty);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(saved));
  EXPECT_EQ(std::get<storage::SaveOutcome>(saved), storage::SaveOutcome::Saved);
  EXPECT_TRUE(empty.dirty());  // Only the caller may acknowledge a successful save.
  ASSERT_TRUE(fs::is_regular_file(file));
  const auto loaded = store.load();
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(loaded));
  EXPECT_TRUE(std::get<storage::MetadataCatalog>(loaded).query().empty());
  EXPECT_FALSE(std::get<storage::MetadataCatalog>(loaded).dirty());
  const auto encoded = codec::encode_index(empty);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(read_bytes(file), std::get<std::string>(encoded));
  expect_no_data_roots();

  empty.mark_clean();
  ASSERT_TRUE(fs::remove(file));
  const auto no_access = store.save(empty);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(no_access));
  EXPECT_EQ(std::get<storage::SaveOutcome>(no_access),
            storage::SaveOutcome::SkippedClean);
  EXPECT_FALSE(fs::exists(file));
}

TEST_F(IndexStoreTest, ReplacesOldCacheAndRestoresCompleteCatalog) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  const auto first_save = store.save(catalog);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(first_save));
  EXPECT_EQ(std::get<storage::SaveOutcome>(first_save), storage::SaveOutcome::Saved);
  const auto first_bytes = read_bytes(file);
  catalog.mark_clean();

  auto added = std::get<storage::LogMetadata>(*catalog.find_entry(
      id("17183823-2574-4bfd-b411-99ed177d3e43")));
  added.id = id("67183823-2574-4bfd-b411-99ed177d3e43");
  added.relative_path = "logs/second.md";
  added.title = "Another Log";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::EntryMetadata{added})));
  const auto second_save = store.save(catalog);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(second_save));
  EXPECT_EQ(std::get<storage::SaveOutcome>(second_save), storage::SaveOutcome::Saved);
  EXPECT_TRUE(catalog.dirty());
  const auto encoded = codec::encode_index(catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  EXPECT_EQ(read_bytes(file), std::get<std::string>(encoded));
  EXPECT_NE(read_bytes(file), first_bytes);

  const auto loaded = store.load();
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(loaded));
  const auto& restored = std::get<storage::MetadataCatalog>(loaded);
  EXPECT_FALSE(restored.dirty());
  EXPECT_EQ(restored.query().size(), 6U);
  const auto reencoded = codec::encode_index(restored);
  ASSERT_TRUE(std::holds_alternative<std::string>(reencoded));
  EXPECT_EQ(std::get<std::string>(reencoded), read_bytes(file));
  EXPECT_EQ(restored.list_root(storage::EntryType::Log).entries.size(), 2U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Memo).directories.size(), 1U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Todo).directories.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      restored.list_directory(core::DirectoryKind::Memo,
          id("87183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      restored.list_directory(core::DirectoryKind::Todo,
          id("97183823-2574-4bfd-b411-99ed177d3e43"))).size(), 1U);
  expect_no_data_roots();

  ASSERT_TRUE(fs::remove(file));
  EXPECT_EQ(restored.query().size(), 6U);
  EXPECT_FALSE(fs::exists(file));
  expect_error(store.load(), storage::IndexStoreErrorCode::Missing);
  EXPECT_FALSE(fs::exists(file));
}

TEST_F(IndexStoreTest, DistinguishesMissingCorruptUnsupportedAndOversizedCache) {
  storage::NormalIndexStore store{paths()};
  expect_error(store.load(), storage::IndexStoreErrorCode::Missing);
  EXPECT_FALSE(fs::exists(state));
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, "bad MessagePack"));
  const auto corrupt = store.load();
  expect_error(corrupt, storage::IndexStoreErrorCode::Corrupt);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(corrupt));
  ASSERT_TRUE(std::get<storage::IndexStoreError>(corrupt).codec_error.has_value());

  msgpack::sbuffer packed;
  msgpack::packer<msgpack::sbuffer> packer{packed};
  packer.pack_map(3);
  packer.pack(std::string{"directories"}); packer.pack_array(0);
  packer.pack(std::string{"entries"}); packer.pack_array(0);
  packer.pack(std::string{"schema_version"}); packer.pack_uint64(2);
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, {packed.data(), packed.size()}));
  const auto unsupported = store.load();
  expect_error(unsupported, storage::IndexStoreErrorCode::UnsupportedVersion);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(unsupported));
  ASSERT_TRUE(std::get<storage::IndexStoreError>(unsupported).codec_error.has_value());

  const std::string oversized(64u * 1024u * 1024u + 1, '\0');
  ASSERT_NO_FATAL_FAILURE(write_bytes(file, oversized));
  expect_error(store.load(), storage::IndexStoreErrorCode::LimitExceeded);
  expect_no_data_roots();
}

TEST_F(IndexStoreTest, ReportsPathResolutionIoErrorDistinctly) {
  auto invalid = paths();
  invalid.state_root = temporary.path() / std::string(8192, 'x');
  storage::NormalIndexStore store{invalid};
  const auto loaded = store.load();
  expect_error(loaded, storage::IndexStoreErrorCode::IoError);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(loaded));
  ASSERT_TRUE(std::get<storage::IndexStoreError>(loaded).storage_error.has_value());
  EXPECT_TRUE(std::get<storage::IndexStoreError>(loaded)
                  .storage_error->system_error);
  expect_no_data_roots();
  EXPECT_FALSE(fs::exists(state));
}

TEST_F(IndexStoreTest, RejectsOverlappingConfiguredRootsWithoutMutation) {
  auto invalid = paths();
  invalid.vault_root = invalid.normal_root;
  storage::NormalIndexStore store{invalid};
  auto catalog = make_catalog();
  expect_error(store.load(), storage::IndexStoreErrorCode::InvalidPath);
  expect_error(store.save(catalog), storage::IndexStoreErrorCode::InvalidPath);
  EXPECT_TRUE(catalog.dirty());
  EXPECT_FALSE(fs::exists(state));
  expect_no_data_roots();
}

TEST_F(IndexStoreTest, EncodingFailurePreservesExistingCacheAndDirtyState) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(store.save(catalog)));
  const auto original = read_bytes(file);
  catalog.mark_clean();
  auto log = std::get<storage::LogMetadata>(*catalog.find_entry(
      id("17183823-2574-4bfd-b411-99ed177d3e43")));
  log.log_date.date = std::chrono::year{-1} / 1 / 1;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.update_entry(
      storage::EntryMetadata{log})));
  const auto failed = store.save(catalog);
  expect_error(failed, storage::IndexStoreErrorCode::CodecError);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(failed));
  ASSERT_TRUE(std::get<storage::IndexStoreError>(failed).codec_error.has_value());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(catalog.dirty());
  expect_no_data_roots();
}

TEST_F(IndexStoreTest, PendingTemporaryBlocksReplacementAndPreservesOldBytes) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(store.save(catalog)));
  const auto original = read_bytes(file);
  catalog.mark_clean();
  change_log_title(catalog);
  const auto pending = file.parent_path() /
      ("." + file.filename().string() + ".tmp-" +
       core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write_bytes(pending, "unfinished cache"));
  const auto failed = store.save(catalog);
  expect_error(failed, storage::IndexStoreErrorCode::StorageFailure);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(failed));
  const auto& cause = std::get<storage::IndexStoreError>(failed);
  ASSERT_TRUE(cause.storage_error.has_value());
  EXPECT_EQ(cause.storage_error->code, storage::StorageErrorCode::RecoveryRequired);
  EXPECT_FALSE(cause.storage_error->replacement_completed);
  EXPECT_TRUE(catalog.dirty());
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_EQ(read_bytes(pending), "unfinished cache");
  expect_no_data_roots();
}

TEST_F(IndexStoreTest, ParentFileAndSymlinkFailuresDoNotWriteDataRoots) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  ASSERT_TRUE(fs::create_directories(state));
  const auto blocking_parent = state / "index";
  ASSERT_NO_FATAL_FAILURE(write_bytes(blocking_parent, "not a directory"));
  const auto blocked = store.save(catalog);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(blocked));
  EXPECT_TRUE(catalog.dirty());
  EXPECT_EQ(read_bytes(blocking_parent), "not a directory");
  EXPECT_FALSE(fs::exists(file));
  expect_no_data_roots();

  ASSERT_TRUE(fs::remove(blocking_parent));
  const auto outside = temporary.path() / "outside";
  ASSERT_TRUE(fs::create_directory(outside));
  std::error_code ec;
  fs::create_directory_symlink(outside, blocking_parent, ec);
  if (ec) GTEST_SKIP() << "Cannot create directory symlink: " << ec.message();
  const auto symlinked = store.save(catalog);
  expect_error(symlinked, storage::IndexStoreErrorCode::InvalidPath);
  EXPECT_TRUE(catalog.dirty());
  EXPECT_TRUE(fs::is_empty(outside));
  expect_no_data_roots();
}

TEST_F(IndexStoreTest, SymbolicLinkCacheCannotReplaceUnrelatedFile) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  ASSERT_TRUE(fs::create_directories(file.parent_path()));
  const auto outside = temporary.path() / "outside.msgpack";
  ASSERT_NO_FATAL_FAILURE(write_bytes(outside, "outside bytes"));
  std::error_code ec;
  fs::create_symlink(outside, file, ec);
  if (ec) GTEST_SKIP() << "Cannot create file symlink: " << ec.message();
  const auto failed = store.save(catalog);
  expect_error(failed, storage::IndexStoreErrorCode::InvalidPath);
  EXPECT_EQ(read_bytes(outside), "outside bytes");
  EXPECT_TRUE(catalog.dirty());
  expect_no_data_roots();
}

#ifndef _WIN32
TEST_F(IndexStoreTest, DirectoryLockFailurePreservesExistingCacheAndDirtyFlag) {
  storage::NormalIndexStore store{paths()};
  auto catalog = make_catalog();
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(store.save(catalog)));
  const auto original = read_bytes(file);
  catalog.mark_clean();
  change_log_title(catalog);
  const int descriptor = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) GTEST_SKIP() << "Cannot open cache directory for locking";
  struct LockGuard {
    int fd;
    ~LockGuard() { (void)flock(fd, LOCK_UN); (void)close(fd); }
  } guard{descriptor};
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    GTEST_SKIP() << "Cannot acquire cache directory lock";
  const auto failed = store.save(catalog);
  expect_error(failed, storage::IndexStoreErrorCode::IoError);
  ASSERT_TRUE(std::holds_alternative<storage::IndexStoreError>(failed));
  const auto& cause = std::get<storage::IndexStoreError>(failed);
  ASSERT_TRUE(cause.storage_error.has_value());
  EXPECT_EQ(cause.storage_error->code, storage::StorageErrorCode::IoError);
  EXPECT_FALSE(cause.storage_error->replacement_completed);
  EXPECT_EQ(read_bytes(file), original);
  EXPECT_TRUE(catalog.dirty());
  expect_no_data_roots();
}
#endif

}  // namespace

