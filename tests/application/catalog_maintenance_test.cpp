#include "operations/catalog_maintenance.hpp"
#include "storage/catalog_coordinator.hpp"
#include "operations/access_orchestrator.hpp"
#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <atomic>
#include <thread>

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;
namespace storage = core::storage;
namespace fs = std::filesystem;
class CatalogMaintenance : public testing::Test {
protected:
  fs::path base = fs::weakly_canonical(fs::temp_directory_path()) /
                  ("fmaintenance-" + core::Id::generate().to_string());
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", {}};
  storage::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state", base / "cache",
                               base / "logs", std::nullopt}};
  storage::MetadataCatalog catalog;
  storage::NormalRepository repository{paths.normal_root};
  storage::CatalogCoordinator coordinator{paths, catalog};
  folio::application::internal::CatalogMaintenance& maintenance = coordinator.maintenance();
  core::Directory directory{core::DirectoryKind::Memo, "Folder"};
  core::Memo root{"Root", "body", time}, member{"Member", "body", time};
  void SetUp() override {
    fs::create_directories(paths.normal_root);
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
        directory.kind(), directory.snapshot(), coordinator.directory_creation_callback(),
        coordinator.creation_check())));
    member.set_directory_id(directory.id(), time);
    for (const auto* memo : {&root, &member})
      ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(
          memo->snapshot(), coordinator.memo_creation_callback(), coordinator.creation_check())));
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  app::RefreshScope scope(app::DirectoryRef dir, bool recursive = true) {
    return app::DirectoryScanScope{std::move(dir), recursive ? app::RefreshTraversal::Recursive
                                                             : app::RefreshTraversal::Immediate};
  }
  fs::path path(const core::Id& id) {
    return paths.normal_root /
           std::get<storage::MemoMetadata>(*catalog.read_view().find_record(id)).relative_path;
  }
};
TEST_F(CatalogMaintenance, CountsAllLayoutsWithoutClassifyingFiles) {
  for (auto type : {core::RecordType::Log, core::RecordType::Memo, core::RecordType::Todo})
    EXPECT_EQ(std::get<app::ConsistencyCheckResult>(maintenance.check(coordinator, type)),
              app::ConsistencyCheckResult::NoAnomalyDetected);
  EXPECT_EQ(std::get<app::ConsistencyCheckResult>(maintenance.check(coordinator, directory.id())),
            app::ConsistencyCheckResult::NoAnomalyDetected);
  std::ofstream(paths.normal_root / "memos" / "unrelated") << "unrelated";
  EXPECT_EQ(std::get<app::ConsistencyCheckResult>(maintenance.check(coordinator, core::RecordType::Memo)),
            app::ConsistencyCheckResult::NoAnomalyDetected);
  folio::application::internal::CatalogMaintenance next_session(paths);
  EXPECT_EQ(std::get<app::ConsistencyCheckResult>(next_session.check(coordinator, core::RecordType::Memo)),
            app::ConsistencyCheckResult::AnomalyDetected);
  fs::remove(paths.normal_root / "memos" / directory.id().to_string() / ".folio-dir.toml");
  EXPECT_EQ(std::get<app::ConsistencyCheckResult>(next_session.check(coordinator, directory.id())),
            app::ConsistencyCheckResult::AnomalyDetected);
  fs::remove_all(paths.normal_root / "memos" / directory.id().to_string());
  folio::application::internal::CatalogMaintenance missing_session(paths);
  EXPECT_TRUE(std::holds_alternative<app::ApplicationError>(missing_session.check(coordinator, directory.id())));
  fs::create_directories(paths.normal_root / "memos" / directory.id().to_string());
  EXPECT_TRUE(std::holds_alternative<app::ApplicationError>(missing_session.check(coordinator, directory.id())));
}
TEST_F(CatalogMaintenance, ImmediateRootPreservesUnscannedMembersAndRetainedViews) {
  auto retained = coordinator.read_state().view;
  fs::remove(path(root.id()));
  fs::remove(path(member.id()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      maintenance.refresh(coordinator, scope(core::RecordType::Memo, false))));
  EXPECT_FALSE(catalog.read_view().contains_entry(root.id()));
  EXPECT_TRUE(catalog.read_view().contains_entry(member.id()));
  EXPECT_TRUE(retained.contains_entry(root.id()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, scope(directory.id()))));
  EXPECT_FALSE(catalog.read_view().contains_entry(member.id()));
  EXPECT_TRUE(catalog.read_view().contains_entry(directory.id()));
  EXPECT_TRUE(catalog.unpersisted());
}
TEST_F(CatalogMaintenance, MissingDirectoryRequiresFullRefreshWithoutPublishing) {
  fs::remove_all(paths.normal_root / "memos" / directory.id().to_string());
  auto response = maintenance.refresh(coordinator, scope(core::RecordType::Memo, false));
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(response));
  EXPECT_EQ(std::get<app::ApplicationError>(response).reason,
            app::ErrorReason::FullRefreshRequired);
  EXPECT_TRUE(catalog.read_view().contains_entry(member.id()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, {})));
  EXPECT_FALSE(catalog.read_view().contains_entry(member.id()));
}
TEST_F(CatalogMaintenance, CrossScopeIdentityConflictRequiresFullRefresh) {
  fs::rename(path(member.id()), paths.normal_root / "memos" / "relocated.md");
  // Authoritative membership still names the Directory; a valid relocated file
  // must be written with Root membership, independently of the old Catalog.
  auto snapshot = member.snapshot();
  snapshot.directory_id.reset();
  fs::remove(paths.normal_root / "memos" / "relocated.md");
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(snapshot)));
  auto response = maintenance.refresh(coordinator, scope(core::RecordType::Memo, false));
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(response));
  EXPECT_EQ(std::get<app::ApplicationError>(response).reason,
            app::ErrorReason::FullRefreshRequired);
  EXPECT_TRUE(catalog.read_view().contains_entry(member.id()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, {})));
  EXPECT_FALSE(std::get<storage::MemoMetadata>(*catalog.read_view().find_record(member.id()))
                   .metadata.directory_id);
}
TEST_F(CatalogMaintenance, FailedScanPreservesCatalogAndAccess) {
  std::ofstream(path(root.id()), std::ios::trunc) << "invalid persisted Memo";
  auto response = maintenance.refresh(coordinator, scope(core::RecordType::Memo));
  EXPECT_TRUE(std::holds_alternative<app::ApplicationError>(response));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(catalog.read_view().contains_entry(root.id()));
  ipc::AccessOrchestrator access{catalog, repository, coordinator};
  EXPECT_TRUE(std::holds_alternative<app::Annotated<app::BrowseResult>>(
      access.handle(ipc::BrowseRequest{core::RecordType::Memo, {}})));
}
TEST_F(CatalogMaintenance, ScopedRefreshCannotClearInconsistentButFullCan) {
  storage::RepositoryMutationError failure{
      storage::MutationFailureStage::AuthoritativeStateMayHaveChanged,
      storage::StorageError{storage::StorageErrorCode::OperationFailure, {}, "test", {}, true},
      {}};
  coordinator.memo_update_callback()(std::cref(failure));
  ASSERT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, scope(directory.id()))));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.shutdown_save()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, {})));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_FALSE(fs::exists(paths.application.state / "index" / "normal.msgpack"));
}
TEST_F(CatalogMaintenance, OldPreparedLoadRemainsFailedAfterSuccessfulRefresh) {
  ipc::AccessOrchestrator access{catalog, repository, coordinator};
  auto old_load = access.prepare(ipc::LoadMemoRequest{root.id()});
  auto original = path(root.id());
  fs::rename(original, original.parent_path() / "renamed.md");
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, {})));
  auto result = old_load();
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(result));
  EXPECT_EQ(std::get<app::ApplicationError>(result).reason, app::ErrorReason::StaleLocation);
  EXPECT_TRUE(std::holds_alternative<folio::application::Annotated<core::Memo::Snapshot>>(
      access.handle(ipc::LoadMemoRequest{root.id()})));
}
TEST_F(CatalogMaintenance, RecursiveRootRemovesMembersButPreservesOtherModules) {
  core::Todo todo{"Task", "notes", time};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_todo(
      todo.snapshot(), coordinator.todo_creation_callback(), coordinator.creation_check())));
  fs::remove_all(paths.normal_root / "memos" / directory.id().to_string());
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, scope(core::RecordType::Memo))));
  EXPECT_FALSE(catalog.read_view().contains_entry(member.id()));
  EXPECT_FALSE(catalog.read_view().contains_entry(directory.id()));
  EXPECT_TRUE(catalog.read_view().contains_entry(todo.id()));
}
TEST_F(CatalogMaintenance, DirectoryScopeDoesNotReadMalformedSiblingAndFullFailsAtomically) {
  std::ofstream(path(root.id()), std::ios::trunc) << "invalid";
  fs::remove(path(member.id()));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, scope(directory.id()))));
  EXPECT_FALSE(catalog.read_view().contains_entry(member.id()));
  EXPECT_TRUE(catalog.read_view().contains_entry(root.id()));
  EXPECT_TRUE(std::holds_alternative<app::ApplicationError>(maintenance.refresh(coordinator, {})));
  EXPECT_TRUE(catalog.read_view().contains_entry(root.id()));
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}
TEST_F(CatalogMaintenance, DuplicateUuidFailsFullRefreshWithoutPublication) {
  fs::copy_file(path(root.id()), path(root.id()).parent_path() / "duplicate.md");
  auto before = coordinator.read_state().view;
  auto result = maintenance.refresh(coordinator, {});
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(result));
  EXPECT_EQ(std::get<app::ApplicationError>(result).reason, app::ErrorReason::IdentityConflict);
  EXPECT_EQ(catalog.read_view().query().size(), before.query().size());
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}
TEST_F(CatalogMaintenance, ConcurrentCapturesRemainCompleteDuringRefreshPublication) {
  std::atomic<bool> valid{true};
  std::jthread reader([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto state = coordinator.read_state();
      if (state.status != storage::CatalogStatus::Ready ||
          state.view.record_count(core::RecordType::Memo) != 1 ||
          state.view.record_count(core::RecordType::Memo, directory.id()) != 1 ||
          state.view.directory_count(core::DirectoryKind::Memo) != 1)
        valid = false;
    }
  });
  for (int i = 0; i < 8; ++i)
    EXPECT_TRUE(std::holds_alternative<std::monostate>(maintenance.refresh(coordinator, {})));
  reader.request_stop();
  reader.join();
  EXPECT_TRUE(valid.load());
}
TEST(CatalogMaintenanceCodec, RoundTripsScopesAndRejectsMalformedValues) {
  for (const app::RefreshScope& scope :
       {app::RefreshScope{},
        app::RefreshScope{app::DirectoryScanScope{app::DirectoryRef{core::RecordType::Memo},
                                                  app::RefreshTraversal::Immediate}},
        app::RefreshScope{app::DirectoryScanScope{app::DirectoryRef{core::Id::generate()},
                                                  app::RefreshTraversal::Recursive}}}) {
    auto encoded = ipc::codec::encode<ipc::OperationRequest>(ipc::RefreshRequest{scope});
    ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
    auto bytes = std::get<std::string>(encoded);
    auto decoded = ipc::codec::decode<ipc::OperationRequest>(bytes);
    ASSERT_TRUE(std::holds_alternative<ipc::OperationRequest>(decoded));
    EXPECT_EQ(std::get<ipc::RefreshRequest>(std::get<ipc::OperationRequest>(decoded)).scope.index(),
              scope.index());
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
        ipc::codec::decode<ipc::OperationRequest>(bytes + "x")));
    bytes.pop_back();
    EXPECT_TRUE(
        std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::OperationRequest>(bytes)));
  }
  auto invalid =
      ipc::codec::encode<ipc::OperationRequest>(ipc::RefreshRequest{app::DirectoryScanScope{
          app::DirectoryRef{core::RecordType::Memo}, static_cast<app::RefreshTraversal>(7)}});
  ASSERT_TRUE(std::holds_alternative<std::string>(invalid));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::codec::decode<ipc::OperationRequest>(std::get<std::string>(invalid))));
  for (auto result : {app::ConsistencyCheckResult::NoAnomalyDetected,
                      app::ConsistencyCheckResult::AnomalyDetected}) {
    auto bytes = std::get<std::string>(ipc::codec::encode<ipc::OperationResponse>(result));
    EXPECT_EQ(std::get<app::ConsistencyCheckResult>(std::get<ipc::OperationResponse>(
                  ipc::codec::decode<ipc::OperationResponse>(bytes))),
              result);
  }
}
} // namespace
