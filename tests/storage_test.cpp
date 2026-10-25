#include "folio/storage/filesystem.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {

namespace fs = std::filesystem;
namespace storage = folio::storage;
using folio::core::Id;
using folio::core::Timestamp;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    for (unsigned int index = 0; index < 10000; ++index) {
      const auto candidate = fs::temp_directory_path() /
          ("folio-storage-test-" + std::to_string(index));
      if (fs::create_directory(candidate)) {
        path_ = candidate;
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

class StorageFilesystemTest : public testing::Test {
 protected:
  TemporaryDirectory temporary;
};

std::set<fs::path> children(const fs::path& directory) {
  std::set<fs::path> names;
  for (const auto& item : fs::directory_iterator(directory)) {
    names.insert(item.path().filename());
  }
  return names;
}

std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) throw std::runtime_error("Could not open test file");
  std::string bytes{std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}};
  if (input.bad()) throw std::runtime_error("Could not read test file");
  return bytes;
}

void seed_file(const fs::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  ASSERT_FALSE(output.fail());
}

template <typename T>
void expect_storage_error(const storage::Result<T>& result) {
  EXPECT_TRUE(std::holds_alternative<storage::StorageError>(result));
}

TEST_F(StorageFilesystemTest, ValidatesSuppliedPathsWithoutCreatingDirectories) {
  const std::array<fs::path, 4> roots{
      temporary.path() / "normal", temporary.path() / "vault",
      temporary.path() / "state", temporary.path() / "cache"};
  const storage::StoragePaths paths{roots[0], roots[1], roots[2], roots[3]};
  const auto validated = storage::validate_storage_paths(paths);
  ASSERT_TRUE(std::holds_alternative<storage::StoragePaths>(validated));
  const auto& [normal, vault, state, cache] =
      std::get<storage::StoragePaths>(validated);
  EXPECT_EQ(normal, roots[0]);
  EXPECT_EQ(vault, roots[1]);
  EXPECT_EQ(state, roots[2]);
  EXPECT_EQ(cache, roots[3]);
  EXPECT_TRUE(children(temporary.path()).empty());
}

TEST_F(StorageFilesystemTest, RequiresEveryPathRatherThanSupplyingDefaults) {
  expect_storage_error(storage::validate_storage_paths(storage::StoragePaths{}));
  const std::array<fs::path, 4> roots{
      temporary.path() / "normal", temporary.path() / "vault",
      temporary.path() / "state", temporary.path() / "cache"};
  for (std::size_t index = 0; index < roots.size(); ++index) {
    SCOPED_TRACE(index);
    auto incomplete = roots;
    incomplete[index].clear();
    expect_storage_error(storage::validate_storage_paths(
        {incomplete[0], incomplete[1], incomplete[2], incomplete[3]}));
  }
  EXPECT_TRUE(children(temporary.path()).empty());
}

TEST_F(StorageFilesystemTest, RejectsRelativeAndOverlappingRoots) {
  const auto normal = temporary.path() / "normal";
  const auto vault = temporary.path() / "vault";
  const auto state = temporary.path() / "state";
  const auto cache = temporary.path() / "cache";
  expect_storage_error(storage::validate_storage_paths(
      {"relative-normal", vault, state, cache}));
  expect_storage_error(storage::validate_storage_paths(
      {normal, vault, normal / "state", cache}));
  expect_storage_error(storage::validate_storage_paths(
      {normal, vault, state, state / "cache"}));
  EXPECT_TRUE(children(temporary.path()).empty());
}

TEST_F(StorageFilesystemTest, RejectsPathsContainingEmbeddedNul) {
  const std::array<fs::path, 4> roots{
      temporary.path() / "normal", temporary.path() / "vault",
      temporary.path() / "state", temporary.path() / "cache"};
  for (std::size_t index = 0; index < roots.size(); ++index) {
    SCOPED_TRACE(index);
    auto invalid = roots;
    invalid[index] /= fs::path{std::string{"bad\0suffix", 10}};
    expect_storage_error(storage::validate_storage_paths(
        {invalid[0], invalid[1], invalid[2], invalid[3]}));
  }
}

TEST(StorageFilenameTest, UsesUtcCreationTimeAndTypeSpecificExtensions) {
  const auto id = Id::parse("47183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(id.has_value());
  using namespace std::chrono;
  const auto midnight = sys_days{year{2000} / 3 / 1};
  const std::array<std::pair<Timestamp, std::string>, 4> cases{{
      {Timestamp{milliseconds{-1}}, "1969-12-31_235959_"},
      {Timestamp{}, "1970-01-01_000000_"},
      {midnight - milliseconds{1}, "2000-02-29_235959_"},
      {midnight, "2000-03-01_000000_"}}};
  for (const auto& [created, prefix] : cases) {
    for (const auto type : {storage::EntryType::Log, storage::EntryType::Memo,
                            storage::EntryType::Todo}) {
      const auto filename = storage::normal_entry_filename(type, *id, created);
      ASSERT_TRUE(std::holds_alternative<std::string>(filename));
      const auto extension = type == storage::EntryType::Todo ? ".toml" : ".md";
      EXPECT_EQ(std::get<std::string>(filename), prefix + id->to_string() + extension);
    }
  }
  expect_storage_error(storage::normal_entry_filename(
      static_cast<storage::EntryType>(999), *id, Timestamp{}));
}

TEST(StorageFilenameTest, HasNoDependenceOnUnicodeTitles) {
  const auto id = Id::parse("47183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(id.has_value());
  // Titles cannot enter the filename API; changing a model title keeps these inputs.
  for (const auto* title : {"研究メモ", "café / 旅行", ""}) {
    SCOPED_TRACE(title);
    const auto result = storage::normal_entry_filename(
        storage::EntryType::Memo, *id, Timestamp{});
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "1970-01-01_000000_47183823-2574-4bfd-b411-99ed177d3e43.md");
  }
}

TEST_F(StorageFilesystemTest, AtomicallyCreatesAndReplacesExactBytes) {
  const auto target = temporary.path() / "entry.md";
  const std::array<std::string, 4> contents{
      "Original content", std::string{"a\0b\r\n", 5}, "", "# 你好 — café\n"};
  for (const auto& bytes : contents) {
    const auto result = storage::write_file_atomic(target, bytes);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
    EXPECT_EQ(read_bytes(target), bytes);
    EXPECT_EQ(children(temporary.path()), std::set<fs::path>{target.filename()});
  }
}

TEST_F(StorageFilesystemTest, LeavesStaleTemporaryFilesUntouched) {
  const auto target = temporary.path() / "entry.toml";
  for (const auto* name : {"entry.toml.tmp", ".entry.toml.tmp-stale",
                           ".folio-tmp-stale"}) {
    ASSERT_NO_FATAL_FAILURE(seed_file(temporary.path() / name, "stale synthetic data"));
  }
  const auto stale = children(temporary.path());
  const auto result = storage::write_file_atomic(target, "new data");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
  auto expected = stale;
  expected.insert(target.filename());
  EXPECT_EQ(children(temporary.path()), expected);
  EXPECT_EQ(read_bytes(target), "new data");
  for (const auto& name : stale) {
    EXPECT_EQ(read_bytes(temporary.path() / name), "stale synthetic data");
  }
}

TEST_F(StorageFilesystemTest, FailedReplacementPreservesTargetAndCleansTemporaryFiles) {
  const auto target = temporary.path() / "entry.md";
  ASSERT_TRUE(fs::create_directory(target));
  ASSERT_NO_FATAL_FAILURE(seed_file(target / "old-content", "preserve me"));
  ASSERT_NO_FATAL_FAILURE(seed_file(temporary.path() / "entry.md.tmp", "stale"));
  const auto before = children(temporary.path());
  expect_storage_error(storage::write_file_atomic(target, "replacement"));
  EXPECT_TRUE(fs::is_directory(target));
  EXPECT_EQ(read_bytes(target / "old-content"), "preserve me");
  EXPECT_EQ(read_bytes(temporary.path() / "entry.md.tmp"), "stale");
  EXPECT_EQ(children(temporary.path()), before);
  EXPECT_EQ(children(target), std::set<fs::path>{"old-content"});
}

TEST_F(StorageFilesystemTest, InvalidParentPreservesExistingFileAndLeavesNoTemporaryFiles) {
  const auto parent = temporary.path() / "parent-file";
  ASSERT_NO_FATAL_FAILURE(seed_file(parent, "old bytes"));
  const auto before = children(temporary.path());
  expect_storage_error(storage::write_file_atomic(parent / "entry.md", "new bytes"));
  EXPECT_EQ(read_bytes(parent), "old bytes");
  EXPECT_EQ(children(temporary.path()), before);
}

TEST_F(StorageFilesystemTest, RejectsSymbolicLinkDestination) {
  const auto original = temporary.path() / "original.md";
  const auto link = temporary.path() / "link.md";
  ASSERT_NO_FATAL_FAILURE(seed_file(original, "preserve me"));
  std::error_code ec;
  fs::create_symlink(original, link, ec);
  if (ec) GTEST_SKIP() << "Cannot create a symbolic link: " << ec.message();
  const auto before = children(temporary.path());
  expect_storage_error(storage::write_file_atomic(link, "replacement"));
  EXPECT_TRUE(fs::is_symlink(link));
  EXPECT_EQ(read_bytes(original), "preserve me");
  EXPECT_EQ(children(temporary.path()), before);
}

TEST_F(StorageFilesystemTest, RejectsEmptyTargetWithoutCreatingFiles) {
  expect_storage_error(storage::write_file_atomic(fs::path{}, "new bytes"));
  EXPECT_TRUE(children(temporary.path()).empty());
}

TEST_F(StorageFilesystemTest, EmbeddedNulTargetCannotOverwriteExistingFile) {
  const auto target = temporary.path() / "entry.md";
  ASSERT_NO_FATAL_FAILURE(seed_file(target, "old bytes"));
  auto invalid_name = target.native();
  invalid_name.push_back(fs::path::value_type{});
  invalid_name += fs::path{"suffix"}.native();
  const auto before = children(temporary.path());
  expect_storage_error(storage::write_file_atomic(fs::path{invalid_name}, "new bytes"));
  EXPECT_EQ(read_bytes(target), "old bytes");
  EXPECT_EQ(children(temporary.path()), before);
}

}  // namespace
