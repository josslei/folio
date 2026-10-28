#include "storage/index_codec.hpp"
#include "storage/metadata_catalog.hpp"
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
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;

class CatalogRebuildTest : public testing::Test {
 protected:
  CatalogRebuildTest() {
    bool reserved = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      sandbox = fs::temp_directory_path() /
          ("folio-catalog-rebuild-test-" + core::Id::generate().to_string());
      if (fs::create_directory(sandbox)) {
        reserved = true;
        break;
      }
    }
    if (!reserved)
      throw std::runtime_error("Could not reserve test directory");
    sandbox = fs::canonical(sandbox);
    root = sandbox / "normal";
  }
  ~CatalogRebuildTest() override {
    std::error_code ignored;
    fs::remove_all(sandbox, ignored);
  }
  fs::path sandbox;
  fs::path root;

  static core::Id id(const char* value) {
    const auto parsed = core::Id::parse(value);
    if (!parsed) throw std::runtime_error("Invalid test UUID");
    return *parsed;
  }
  static core::TimestampContext when() {
    return {core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
                std::chrono::seconds{1800000000} + std::chrono::nanoseconds{123456000})},
            "Asia/Shanghai", "上海"};
  }
  static core::Log::Snapshot log() {
    return {id("17183823-2574-4bfd-b411-99ed177d3e43"), when(), when(),
            "旅程 Log", "private body", {{std::chrono::year{2027} / 2 / 3}, "UTC", "東京"},
            {{"daily"}, {"旅行"}}, core::Privacy::Normal};
  }
  static core::Memo::Snapshot memo(std::optional<core::Id> directory = std::nullopt) {
    return {id("27183823-2574-4bfd-b411-99ed177d3e43"), when(), when(),
            "Café Memo", "private body", {{"research"}}, directory, core::Privacy::Normal};
  }
  static core::Todo::Snapshot todo(std::optional<core::Id> directory = std::nullopt) {
    return {id("37183823-2574-4bfd-b411-99ed177d3e43"), when(), when(),
            "計画 Todo", "private notes", core::TodoStatus::Pending,
            core::TodoPriority::High,
            core::DueDate{core::TimestampContext{core::Timestamp{std::chrono::seconds{1900000000}},
                                                 "UTC", std::nullopt}},
            directory, core::Privacy::Normal, {{"Book train", false}}};
  }
  static core::Directory::Snapshot memo_directory() {
    return {id("47183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Memo,
            "研究 / Café"};
  }
  static core::Directory::Snapshot todo_directory() {
    return {id("57183823-2574-4bfd-b411-99ed177d3e43"), core::DirectoryKind::Todo,
            "任务"};
  }
  static std::string bytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not read test file");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }
  static void write(const fs::path& file, const std::string& content) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    ASSERT_FALSE(stream.fail());
  }
  static void replace_in_file(const fs::path& file, const std::string& from,
                              const std::string& to) {
    auto text = bytes(file);
    const auto position = text.find(from);
    ASSERT_NE(position, std::string::npos);
    text.replace(position, from.size(), to);
    ASSERT_NO_FATAL_FAILURE(write(file, text));
  }
  static void error_code(const ::folio::core::internal::Result<storage::NormalRepositoryScanResult, storage::StorageError>& result,
                         storage::StorageErrorCode code) {
    ASSERT_TRUE(std::holds_alternative<storage::StorageError>(result));
    EXPECT_EQ(std::get<storage::StorageError>(result).code, code);
  }
};

TEST_F(CatalogRebuildTest, MissingRootEnumeratesEmptyWithoutWritingAnything) {
  storage::NormalRepository repository{root};
  const auto result = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::NormalRepositoryScanResult>(result));
  const auto& inventory = std::get<storage::NormalRepositoryScanResult>(result);
  EXPECT_TRUE(inventory.logs.empty());
  EXPECT_TRUE(inventory.memos.empty());
  EXPECT_TRUE(inventory.todos.empty());
  EXPECT_TRUE(inventory.directories.empty());
  EXPECT_TRUE(inventory.diagnostics.empty());
  EXPECT_FALSE(fs::exists(root));
  ASSERT_TRUE(fs::create_directory(root));
  const auto existing = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::NormalRepositoryScanResult>(existing));
  EXPECT_TRUE(std::get<storage::NormalRepositoryScanResult>(existing).logs.empty());
  EXPECT_TRUE(std::get<storage::NormalRepositoryScanResult>(existing).memos.empty());
  EXPECT_TRUE(std::get<storage::NormalRepositoryScanResult>(existing).todos.empty());
  EXPECT_TRUE(std::get<storage::NormalRepositoryScanResult>(existing).directories.empty());
  EXPECT_TRUE(fs::is_empty(root));
}

TEST_F(CatalogRebuildTest, EnumeratesCompleteMetadataAndActualRenamedLocators) {
  storage::NormalRepository repository{root};
  const auto md = memo_directory();
  const auto td = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, md)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Todo, td)));
  const auto l = log();
  const auto m = memo(md.id);
  const auto t = todo(td.id);
  auto root_memo = memo();
  root_memo.id = id("67183823-2574-4bfd-b411-99ed177d3e43");
  auto root_todo = todo();
  root_todo.id = id("77183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(l)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(m)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(t)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(root_memo)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(root_todo)));
  const auto root_memo_path = repository.locate_entry(storage::EntryType::Memo, root_memo.id);
  const auto root_todo_path = repository.locate_entry(storage::EntryType::Todo, root_todo.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(root_memo_path));
  ASSERT_TRUE(std::holds_alternative<fs::path>(root_todo_path));
  const auto log_locator = repository.locate_entry(storage::EntryType::Log, l.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(log_locator));
  const fs::path renamed_log{"logs/manual-name.md"};
  fs::rename(root / std::get<fs::path>(log_locator), root / renamed_log);
  const auto memo_locator = repository.locate_entry(storage::EntryType::Memo, m.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_locator));
  const fs::path renamed_memo = fs::path{"memos"} / md.id.to_string() / "draft.tmp-not-atomic.md";
  fs::rename(root / std::get<fs::path>(memo_locator), root / renamed_memo);
  const auto todo_locator = repository.locate_entry(storage::EntryType::Todo, t.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(todo_locator));
  const fs::path renamed_todo = fs::path{"todos"} / td.id.to_string() / "manual.toml";
  fs::rename(root / std::get<fs::path>(todo_locator), root / renamed_todo);
  const auto before_log = bytes(root / renamed_log);
  const auto before_memo = bytes(root / renamed_memo);
  const auto before_todo = bytes(root / renamed_todo);
  const auto before_memo_directory = bytes(root / "memos" / md.id.to_string() /
                                           ".folio-dir.toml");
  const auto before_todo_directory = bytes(root / "todos" / td.id.to_string() /
                                           ".folio-dir.toml");

  const auto result = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::NormalRepositoryScanResult>(result));
  const auto& inventory = std::get<storage::NormalRepositoryScanResult>(result);
  ASSERT_EQ(inventory.logs.size(), 1U);
  ASSERT_EQ(inventory.memos.size(), 2U);
  ASSERT_EQ(inventory.todos.size(), 2U);
  ASSERT_EQ(inventory.directories.size(), 2U);
  EXPECT_TRUE(inventory.diagnostics.empty());
  storage::MetadataCatalog catalog;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.rebuild(
      {inventory.logs.begin(), inventory.logs.end()},
      {inventory.memos.begin(), inventory.memos.end()},
      {inventory.todos.begin(), inventory.todos.end()},
      {inventory.directories.begin(), inventory.directories.end()})));
  EXPECT_TRUE(catalog.unpersisted());
  storage::MetadataCatalog expected;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_directory(
      storage::metadata_from(md, fs::path{"memos"} / md.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_directory(
      storage::metadata_from(td, fs::path{"todos"} / td.id.to_string()))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_entry(
      storage::metadata_from(l, renamed_log))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_entry(
      storage::metadata_from(m, renamed_memo))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_entry(
      storage::metadata_from(t, renamed_todo))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_entry(
      storage::metadata_from(root_memo, std::get<fs::path>(root_memo_path)))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(expected.insert_entry(
      storage::metadata_from(root_todo, std::get<fs::path>(root_todo_path)))));
  const auto encoded_actual = storage::index_codec::encode_index(catalog);
  const auto encoded_expected = storage::index_codec::encode_index(expected);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded_actual));
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded_expected));
  EXPECT_EQ(std::get<std::string>(encoded_actual),
            std::get<std::string>(encoded_expected));
  const auto loaded_log = catalog.find_entry(l.id);
  ASSERT_TRUE(loaded_log.has_value());
  const auto& log_metadata = std::get<storage::LogMetadata>(*loaded_log);
  EXPECT_EQ(log_metadata.relative_path, renamed_log);
  EXPECT_EQ(log_metadata.title, l.title);
  EXPECT_EQ(log_metadata.tags, l.tags);
  EXPECT_EQ(log_metadata.created.timestamp, l.created.timestamp);
  EXPECT_EQ(log_metadata.created.timezone, l.created.timezone);
  EXPECT_EQ(log_metadata.created.location, l.created.location);
  EXPECT_EQ(log_metadata.log_date.date, l.log_date.date);
  const auto loaded_memo = catalog.find_entry(m.id);
  ASSERT_TRUE(loaded_memo.has_value());
  const auto& memo_metadata = std::get<storage::MemoMetadata>(*loaded_memo);
  EXPECT_EQ(memo_metadata.relative_path, renamed_memo);
  EXPECT_EQ(memo_metadata.directory_id, md.id);
  EXPECT_EQ(memo_metadata.title, m.title);
  const auto loaded_todo = catalog.find_entry(t.id);
  ASSERT_TRUE(loaded_todo.has_value());
  const auto& todo_metadata = std::get<storage::TodoMetadata>(*loaded_todo);
  EXPECT_EQ(todo_metadata.relative_path, renamed_todo);
  EXPECT_EQ(todo_metadata.directory_id, td.id);
  EXPECT_EQ(todo_metadata.title, t.title);
  EXPECT_EQ(todo_metadata.status, t.status);
  EXPECT_EQ(todo_metadata.priority, t.priority);
  EXPECT_EQ(todo_metadata.due_date.has_value(), t.due_date.has_value());
  EXPECT_EQ(catalog.list_root(storage::EntryType::Log).entries.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Memo).directories.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).directories.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Memo).entries.size(), 1U);
  EXPECT_EQ(catalog.list_root(storage::EntryType::Todo).entries.size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      catalog.list_directory(core::DirectoryKind::Memo, md.id)).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(
      catalog.list_directory(core::DirectoryKind::Todo, td.id)).size(), 1U);
  EXPECT_EQ(bytes(root / renamed_log), before_log);
  EXPECT_EQ(bytes(root / renamed_memo), before_memo);
  EXPECT_EQ(bytes(root / renamed_todo), before_todo);
  EXPECT_EQ(bytes(root / "memos" / md.id.to_string() / ".folio-dir.toml"),
            before_memo_directory);
  EXPECT_EQ(bytes(root / "todos" / td.id.to_string() / ".folio-dir.toml"),
            before_todo_directory);
  EXPECT_FALSE(fs::exists(sandbox / "state"));
}

TEST_F(CatalogRebuildTest, AtomicTemporariesAreIgnoredAndUnrelatedObjectsDiagnosed) {
  storage::NormalRepository repository{root};
  const auto l = log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(l)));
  const auto locator = repository.locate_entry(storage::EntryType::Log, l.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto formal = root / std::get<fs::path>(locator);
  const auto pending = formal.parent_path() /
      ("." + formal.filename().string() + ".tmp-" + core::Id::generate().to_string());
  ASSERT_NO_FATAL_FAILURE(write(pending, bytes(formal)));
  ASSERT_NO_FATAL_FAILURE(write(root / "logs" / "unrelated.txt", "not an Entry"));
  ASSERT_TRUE(fs::create_directory(root / "logs" / "unrelated-folder"));
  const auto result = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::NormalRepositoryScanResult>(result));
  const auto& inventory = std::get<storage::NormalRepositoryScanResult>(result);
  EXPECT_EQ(inventory.logs.size(), 1U);
  EXPECT_TRUE(inventory.memos.empty());
  EXPECT_TRUE(inventory.todos.empty());
  EXPECT_EQ(inventory.diagnostics.size(), 2U);
  for (const auto& diagnostic : inventory.diagnostics) {
    EXPECT_FALSE(diagnostic.path.empty());
    EXPECT_NE(diagnostic.path, pending);
  }
  EXPECT_EQ(bytes(pending), bytes(formal));
}

TEST_F(CatalogRebuildTest, DuplicateStoredIdentityIsFatalEvenUnderDifferentFilenames) {
  storage::NormalRepository repository{root};
  const auto l = log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(l)));
  const auto locator = repository.locate_entry(storage::EntryType::Log, l.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto original = root / std::get<fs::path>(locator);
  const auto duplicate = root / "logs" / "other.md";
  ASSERT_NO_FATAL_FAILURE(write(duplicate, bytes(original)));
  error_code(repository.scan(), storage::StorageErrorCode::DuplicateId);
  EXPECT_EQ(bytes(duplicate), bytes(original));
}

TEST_F(CatalogRebuildTest, MalformedFormalAndMissingDirectoryMetadataAreFatal) {
  storage::NormalRepository repository{root};
  ASSERT_TRUE(fs::create_directories(root / "logs"));
  const auto malformed = root / "logs" / "bad.md";
  ASSERT_NO_FATAL_FAILURE(write(malformed, "invalid front matter"));
  error_code(repository.scan(), storage::StorageErrorCode::CodecError);
  ASSERT_TRUE(fs::remove(malformed));
  const auto md = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, md)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo(md.id))));
  const auto metadata = root / "memos" / md.id.to_string() / ".folio-dir.toml";
  ASSERT_TRUE(fs::remove(metadata));
  const auto failure = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(failure));
  EXPECT_FALSE(fs::exists(metadata));
}

TEST_F(CatalogRebuildTest, DirectorySchemaKindAndIdentityErrorsAreFatal) {
  storage::NormalRepository repository{root};
  const auto md = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, md)));
  const auto metadata = root / "memos" / md.id.to_string() / ".folio-dir.toml";
  const auto original = bytes(metadata);
  ASSERT_NO_FATAL_FAILURE(replace_in_file(metadata, "schema_version = 1", "schema_version = 2"));
  error_code(repository.scan(), storage::StorageErrorCode::CodecError);
  ASSERT_NO_FATAL_FAILURE(write(metadata, original));
  ASSERT_NO_FATAL_FAILURE(replace_in_file(metadata, "kind = 'Memo'", "kind = 'Todo'"));
  error_code(repository.scan(), storage::StorageErrorCode::CodecError);
  ASSERT_NO_FATAL_FAILURE(write(metadata, original));
  ASSERT_NO_FATAL_FAILURE(replace_in_file(metadata, md.id.to_string(),
                                          id("67183823-2574-4bfd-b411-99ed177d3e43").to_string()));
  const auto mismatch = repository.scan();
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(mismatch));
  ASSERT_NO_FATAL_FAILURE(write(metadata, original));

  auto td = todo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Todo, td)));
  const auto old_todo_folder = root / "todos" / td.id.to_string();
  const auto duplicate_folder = root / "todos" / md.id.to_string();
  fs::rename(old_todo_folder, duplicate_folder);
  ASSERT_NO_FATAL_FAILURE(replace_in_file(duplicate_folder / ".folio-dir.toml",
                                          td.id.to_string(), md.id.to_string()));
  error_code(repository.scan(), storage::StorageErrorCode::DuplicateId);
}

TEST_F(CatalogRebuildTest, EntrySchemaAndPhysicalDirectoryMembershipAreValidated) {
  storage::NormalRepository repository{root};
  const auto l = log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(l)));
  const auto locator = repository.locate_entry(storage::EntryType::Log, l.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto log_path = root / std::get<fs::path>(locator);
  const auto original = bytes(log_path);
  ASSERT_NO_FATAL_FAILURE(replace_in_file(log_path, "schema_version = 1", "schema_version = 2"));
  error_code(repository.scan(), storage::StorageErrorCode::CodecError);
  ASSERT_NO_FATAL_FAILURE(write(log_path, original));
  const auto md = memo_directory();
  auto other = md;
  other.id = id("67183823-2574-4bfd-b411-99ed177d3e43");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, md)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, other)));
  const auto m = memo(md.id);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(m)));
  const auto memo_locator = repository.locate_entry(storage::EntryType::Memo, m.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(memo_locator));
  const auto source = root / std::get<fs::path>(memo_locator);
  const auto destination = root / "memos" / other.id.to_string() / source.filename();
  fs::rename(source, destination);
  error_code(repository.scan(), storage::StorageErrorCode::IdentityMismatch);
  EXPECT_TRUE(fs::is_regular_file(destination));
}

TEST_F(CatalogRebuildTest, ModuleRootRegularFileCannotBeEnumeratedAsDirectory) {
  ASSERT_TRUE(fs::create_directory(root));
  ASSERT_NO_FATAL_FAILURE(write(root / "logs", "blocking module"));
  storage::NormalRepository repository{root};
  error_code(repository.scan(), storage::StorageErrorCode::InvalidPath);
  EXPECT_EQ(bytes(root / "logs"), "blocking module");
}

TEST_F(CatalogRebuildTest, NestedAndSymlinkedFormalStructuresAreFatal) {
  storage::NormalRepository repository{root};
  ASSERT_TRUE(fs::create_directories(root / "logs" / "nested"));
  ASSERT_NO_FATAL_FAILURE(write(root / "logs" / "nested" / "entry.md", "bad"));
  error_code(repository.scan(), storage::StorageErrorCode::RecoveryRequired);
  ASSERT_TRUE(fs::remove_all(root / "logs" / "nested") > 0);
  const auto l = log();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(l)));
  const auto locator = repository.locate_entry(storage::EntryType::Log, l.id);
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  const auto outside = sandbox / "outside.md";
  ASSERT_NO_FATAL_FAILURE(write(outside, bytes(root / std::get<fs::path>(locator))));
  std::error_code ec;
  fs::create_symlink(outside, root / "logs" / "linked.md", ec);
  if (ec) GTEST_SKIP() << "Cannot create file symlink: " << ec.message();
  error_code(repository.scan(), storage::StorageErrorCode::InvalidPath);
  EXPECT_TRUE(fs::is_regular_file(outside));
}

TEST_F(CatalogRebuildTest, NestedMemoEntryAndSymlinkedUuidDirectoryAreFatal) {
  storage::NormalRepository repository{root};
  const auto md = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(core::DirectoryKind::Memo, md)));
  const auto nested = root / "memos" / md.id.to_string() / "nested";
  ASSERT_TRUE(fs::create_directory(nested));
  ASSERT_NO_FATAL_FAILURE(write(nested / "entry.md", "not an Entry"));
  error_code(repository.scan(), storage::StorageErrorCode::RecoveryRequired);
  ASSERT_TRUE(fs::remove_all(nested) > 0);
  const auto outside = sandbox / "outside-directory";
  ASSERT_TRUE(fs::create_directory(outside));
  const auto linked_id = id("67183823-2574-4bfd-b411-99ed177d3e43");
  std::error_code ec;
  fs::create_directory_symlink(outside, root / "memos" / linked_id.to_string(), ec);
  if (ec) GTEST_SKIP() << "Cannot create directory symlink: " << ec.message();
  error_code(repository.scan(), storage::StorageErrorCode::InvalidPath);
  EXPECT_TRUE(fs::is_empty(outside));
}

TEST_F(CatalogRebuildTest, RebuildFailurePreservesOldCatalogAndUnpersistedState) {
  storage::MetadataCatalog catalog;
  const auto old = storage::metadata_from(log(), fs::path{"logs/old.md"});
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(old)));
  catalog.mark_persisted();
  ASSERT_FALSE(catalog.unpersisted());
  auto duplicate_memo = memo();
  duplicate_memo.id = log().id;
  const auto duplicate = catalog.rebuild(
      {std::get<storage::LogMetadata>(old)},
      {std::get<storage::MemoMetadata>(storage::metadata_from(
          duplicate_memo, fs::path{"memos/duplicate.md"}))}, {}, {});
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(duplicate));
  EXPECT_EQ(std::get<storage::StorageError>(duplicate).code,
            storage::StorageErrorCode::DuplicateId);
  EXPECT_TRUE(catalog.find_entry(log().id).has_value());
  EXPECT_FALSE(catalog.unpersisted());
  const auto md = memo_directory();
  auto new_memo = storage::metadata_from(memo(md.id), fs::path{"memos/member.md"});
  const auto bad = catalog.rebuild({}, {std::get<storage::MemoMetadata>(new_memo)}, {}, {});
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(bad));
  EXPECT_TRUE(catalog.find_entry(log().id).has_value());
  EXPECT_FALSE(catalog.find_entry(memo().id).has_value());
  EXPECT_FALSE(catalog.unpersisted());
  const auto cross_kind = catalog.rebuild(
      {}, {}, {std::get<storage::TodoMetadata>(storage::metadata_from(todo(md.id), fs::path{"todos/incorrect.toml"}))},
      {storage::metadata_from(md, fs::path{"memos/group"})});
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(cross_kind));
  EXPECT_TRUE(catalog.find_entry(log().id).has_value());
  EXPECT_FALSE(catalog.find_entry(todo().id).has_value());
  EXPECT_FALSE(catalog.unpersisted());

  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.insert_directory(storage::metadata_from(md, fs::path{"memos/group"}))));
  ASSERT_TRUE(catalog.unpersisted());
  const auto conflict = catalog.rebuild({std::get<storage::LogMetadata>(old), std::get<storage::LogMetadata>(old)}, {}, {}, {});
  ASSERT_TRUE(std::holds_alternative<storage::StorageError>(conflict));
  EXPECT_TRUE(catalog.find_directory(md.id).has_value());
  EXPECT_TRUE(catalog.unpersisted());
}

TEST_F(CatalogRebuildTest, SuccessfulEmptyReplacementClearsMembershipAndMarksUnpersisted) {
  storage::MetadataCatalog catalog;
  const auto md = memo_directory();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.insert_directory(storage::metadata_from(md, fs::path{"memos/group"}))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_entry(
      storage::metadata_from(memo(md.id), fs::path{"memos/group/member.md"}))));
  catalog.mark_persisted();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.rebuild({}, {}, {}, {})));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_TRUE(catalog.query().empty());
  EXPECT_TRUE(catalog.list_directories(core::DirectoryKind::Memo).empty());
  EXPECT_TRUE(catalog.list_root(storage::EntryType::Memo).directories.empty());
  EXPECT_FALSE(catalog.find_entry(memo().id).has_value());
  EXPECT_FALSE(catalog.find_directory(md.id).has_value());
}

}  // namespace
