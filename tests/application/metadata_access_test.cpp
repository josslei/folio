#include "metadata_access.hpp"
#include "storage/catalog_coordinator.hpp"
#include <gtest/gtest.h>
#include <fstream>

namespace {
namespace app = folio::application;
namespace access = folio::application::internal;
namespace core = folio::core;
namespace storage = core::storage;
namespace fs = std::filesystem;

class MetadataAccess : public testing::Test {
 protected:
  fs::path base = fs::weakly_canonical(fs::temp_directory_path()) / ("faccess-" + core::Id::generate().to_string());
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", "fixture"};
  storage::StoragePaths paths{base / "normal", base / "vault",
      {base / "config", base / "data", base / "state", base / "cache", base / "logs", std::nullopt}};
  storage::MetadataCatalog catalog;
  storage::NormalRepository repository{paths.normal_root};
  storage::CatalogCoordinator coordinator{paths, catalog};
  core::Directory directory{core::DirectoryKind::Memo, "Projects"};
  core::Memo root{"Alpha", "root-body", time};
  core::Memo member{"Beta", "member-body", time};
  core::Todo todo{"Task", "notes", time};
  core::Log log{"Day", "log-body", time,
      core::DateContext{std::chrono::year{2027}/1/15, "UTC", std::nullopt}, core::Privacy::Normal};

  void SetUp() override {
    fs::create_directories(paths.normal_root);
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
        directory.kind(), directory.snapshot(), coordinator.directory_creation_callback(), coordinator.creation_check())));
    member.set_directory_id(directory.id(), time);
    root.set_tags({{"work"}}, time);
    todo.add_subtask("subtask", time);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(root.snapshot(), coordinator.memo_creation_callback(), coordinator.creation_check())));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(member.snapshot(), coordinator.memo_creation_callback(), coordinator.creation_check())));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(todo.snapshot(), coordinator.todo_creation_callback(), coordinator.creation_check())));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log.snapshot(), coordinator.log_creation_callback(), coordinator.creation_check())));
  }
  void TearDown() override { std::error_code ignored; fs::remove_all(base, ignored); }
  access::AccessResponse call(access::AccessRequest request) {
    return access::handle_access(request, catalog, repository, coordinator);
  }
  template<class T> T value(access::AccessRequest request) {
    auto result = call(std::move(request));
    if (!std::holds_alternative<T>(result)) {
      ADD_FAILURE() << "Unexpected response alternative " << result.index();
      throw std::runtime_error("Unexpected test response");
    }
    return std::get<T>(std::move(result));
  }
};

TEST_F(MetadataAccess, RootDirectoryAndAllScopesStayDistinct) {
  auto browse = value<app::ApplicationResponse<app::BrowseResult>>(
      access::BrowseRequest{core::RecordType::Memo, {}});
  ASSERT_EQ(browse.value.records.size(), 1u);
  EXPECT_EQ(std::get<core::MemoMetadata>(browse.value.records[0]).id, root.id());
  ASSERT_EQ(browse.value.directories.size(), 1u);
  EXPECT_EQ(browse.value.directories[0].id, directory.id());
  auto contents = value<app::ApplicationResponse<app::BrowseResult>>(
      access::BrowseRequest{directory.id(), {}});
  ASSERT_EQ(contents.value.records.size(), 1u);
  EXPECT_EQ(std::get<core::MemoMetadata>(contents.value.records[0]).id, member.id());
  EXPECT_TRUE(contents.value.directories.empty());
  app::RecordQuery query;
  EXPECT_EQ(value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{query}).value.size(), 4u);
  query.scope = app::RootScope{};
  EXPECT_EQ(value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{query}).value.size(), 3u);
  query.scope = app::DirectoryScope{directory.id()};
  EXPECT_EQ(value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{query}).value.size(), 1u);
}

TEST_F(MetadataAccess, DirectoriesSortByNameThenUuidAndStayTyped) {
  core::Directory earlier{core::DirectoryKind::Memo, "A"};
  core::Directory same{core::DirectoryKind::Memo, "Projects"};
  core::Directory other{core::DirectoryKind::Todo, "A"};
  for (const auto* item : {&earlier, &same, &other})
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
        item->kind(), item->snapshot(), coordinator.directory_creation_callback(), coordinator.creation_check())));
  auto listed = value<app::ApplicationResponse<std::vector<core::DirectoryMetadata>>>(
      access::ListDirectoriesRequest{core::DirectoryKind::Memo});
  ASSERT_EQ(listed.value.size(), 3u);
  EXPECT_EQ(listed.value[0].id, earlier.id());
  EXPECT_LT(listed.value[1].id.to_string(), listed.value[2].id.to_string());
  for (const auto& item : listed.value) EXPECT_EQ(item.kind, core::DirectoryKind::Memo);
}

TEST_F(MetadataAccess, FiltersAndOrderingUseMetadataOnly) {
  app::RecordQuery query;
  query.type = core::RecordType::Memo;
  query.filters.title_contains = "ALP";
  query.filters.tag = core::Tag{"work"};
  auto found = value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{query});
  ASSERT_EQ(found.value.size(), 1u);
  EXPECT_EQ(std::get<core::MemoMetadata>(found.value[0]).id, root.id());
  query.filters = {};
  query.order = {app::MetadataSort::Title, true};
  auto sorted = value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{query});
  ASSERT_EQ(sorted.value.size(), 2u);
  EXPECT_EQ(std::get<core::MemoMetadata>(sorted.value[0]).title, "Beta");
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
}

TEST_F(MetadataAccess, RejectsInvalidEnumsRangesDatesAndIncompatibleScope) {
  std::vector<app::RecordQuery> invalid;
  app::RecordQuery query;
  query.type = static_cast<core::RecordType>(99); invalid.push_back(query);
  query = {}; query.order.sort = static_cast<app::MetadataSort>(99); invalid.push_back(query);
  query = {}; query.filters.created = {time.timestamp + std::chrono::hours{1}, time.timestamp}; invalid.push_back(query);
  query = {}; query.filters.log_date.from = std::chrono::year{2027}/2/30; invalid.push_back(query);
  query = {}; query.type = core::RecordType::Todo; query.scope = app::DirectoryScope{directory.id()}; invalid.push_back(query);
  query = {}; query.filters.todo_status = static_cast<core::TodoStatus>(99); invalid.push_back(query);
  query = {}; query.filters.due_from = core::DateContext{std::chrono::year{2028}/1/1, "UTC", {}};
  query.filters.due_through = core::DateContext{std::chrono::year{2027}/1/1, "UTC", {}}; invalid.push_back(query);
  for (const auto& candidate : invalid) {
    auto result = call(access::QueryRecordsRequest{candidate});
    ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(result));
    EXPECT_EQ(std::get<app::ApplicationError>(result).reason, app::ErrorReason::InvalidQuery);
  }
}

TEST_F(MetadataAccess, CompleteTypedLoadsAndMissingWrongTypeStaleAreDistinct) {
  EXPECT_EQ(value<core::Log::Snapshot>(access::LoadLogRequest{log.id()}).content, "log-body");
  EXPECT_EQ(value<core::Memo::Snapshot>(access::LoadMemoRequest{member.id()}).content, "member-body");
  auto task = value<core::Todo::Snapshot>(access::LoadTodoRequest{todo.id()});
  EXPECT_EQ(task.notes, "notes");
  ASSERT_EQ(task.subtasks.size(), 1u);
  EXPECT_EQ(task.subtasks[0].content, "subtask");
  EXPECT_EQ(value<app::ApplicationError>(access::LoadTodoRequest{root.id()}).reason, app::ErrorReason::WrongRecordType);
  EXPECT_EQ(value<app::ApplicationError>(access::LoadMemoRequest{core::Id::generate()}).reason, app::ErrorReason::NotFound);
  const auto metadata = std::get<storage::MemoMetadata>(*catalog.find_record(root.id()));
  fs::remove(paths.normal_root / metadata.relative_path);
  EXPECT_EQ(value<app::ApplicationError>(access::LoadMemoRequest{root.id()}).reason, app::ErrorReason::StaleLocation);
}

TEST_F(MetadataAccess, TargetedReadsIgnoreUnrelatedMalformedFilesAndDoNotWriteIndex) {
  std::ofstream(paths.normal_root / "memos/unrelated.md") << "not a Folio Record";
  EXPECT_EQ(value<core::Memo::Snapshot>(access::LoadMemoRequest{member.id()}).content, "member-body");
  EXPECT_EQ(value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{{}}).value.size(), 4u);
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  EXPECT_TRUE(catalog.unpersisted());
}

TEST_F(MetadataAccess, InconsistentCatalogProducesWarningAndUnavailableIsAnError) {
  storage::RepositoryMutationError failure{storage::MutationFailureStage::AuthoritativeStateMayHaveChanged,
      folio::io::FileError{folio::io::FileErrorCode::IoError, {}, "synthetic failure", {}, true}};
  coordinator.memo_update_callback()(std::cref(failure));
  auto result = value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(access::QueryRecordsRequest{{}});
  ASSERT_EQ(result.warnings.size(), 1u);
  EXPECT_EQ(result.warnings[0].reason, app::WarningReason::CatalogInconsistent);
  storage::MetadataCatalog unloaded;
  storage::CatalogCoordinator not_started{paths, unloaded};
  auto unavailable = access::handle_access(access::QueryRecordsRequest{{}}, unloaded, repository, not_started);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(unavailable));
  EXPECT_EQ(std::get<app::ApplicationError>(unavailable).reason, app::ErrorReason::MetadataUnavailable);
}

TEST_F(MetadataAccess, MalformedRecordFailureDoesNotPoisonOtherReadsOrFinalizeSession) {
  const auto metadata = std::get<storage::MemoMetadata>(*catalog.find_record(root.id()));
  std::ofstream(paths.normal_root / metadata.relative_path, std::ios::trunc)
      << "malformed authoritative Record";
  const auto failure = value<app::ApplicationError>(access::LoadMemoRequest{root.id()});
  EXPECT_EQ(failure.reason, app::ErrorReason::InvalidData);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_EQ(value<core::Memo::Snapshot>(access::LoadMemoRequest{member.id()}).content,
            "member-body");
  EXPECT_EQ(value<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(
      access::QueryRecordsRequest{{}}).value.size(), 4u);
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
}

TEST_F(MetadataAccess, FinalizedSessionRejectsAccessAndManagedCreation) {
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  EXPECT_EQ(value<app::ApplicationError>(access::LoadMemoRequest{root.id()}).reason,
            app::ErrorReason::MetadataUnavailable);
  EXPECT_EQ(value<app::ApplicationError>(access::QueryRecordsRequest{{}}).reason,
            app::ErrorReason::MetadataUnavailable);
  core::Memo late{"After shutdown", "must not be written", time};
  auto result = repository.create_memo(late.snapshot(), coordinator.memo_creation_callback(),
                                       coordinator.creation_check());
  ASSERT_TRUE(std::holds_alternative<storage::RepositoryMutationError>(result));
  EXPECT_EQ(std::get<storage::RepositoryMutationError>(result).stage,
            storage::MutationFailureStage::BeforeAuthoritativeChange);
  EXPECT_FALSE(catalog.contains_entry(late.id()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Stopped);
  EXPECT_FALSE(fs::exists(paths.application.state / "session.marker"));
}
}  // namespace
