#include "operations/mutation_orchestrator.hpp"
#include "operations/access_orchestrator.hpp"
#include "storage/normal_repository_test_hooks.hpp"
#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <future>
#include "operations/request_scheduler.hpp"
#include "ipc/ipc_server.hpp"
#include "backend_connection_handle.hpp"
#include <folio/ipc/message_transport.hpp>

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;
namespace storage = core::storage;
namespace fs = std::filesystem;

class MutationOrchestrator : public testing::Test {
protected:
#ifdef __APPLE__
  fs::path base =
      fs::path("/private/tmp") / ("fmut-" + core::Id::generate().to_string().substr(0, 8));
#else
  fs::path base = fs::weakly_canonical(fs::temp_directory_path()) /
                  ("fmut-" + core::Id::generate().to_string().substr(0, 8));
#endif
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", "creation"};
  core::TimestampContext modified{time.timestamp + std::chrono::hours{1}, "Asia/Shanghai", "edit"};
  storage::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state", base / "cache",
                               base / "logs", std::nullopt}};
  storage::MetadataCatalog catalog;
  storage::NormalRepository repository{paths.normal_root};
  storage::CatalogCoordinator coordinator{paths, catalog};
  ipc::MutationOrchestrator mutations{catalog, repository, coordinator};
  ipc::Session first = std::make_shared<ipc::ConnectionSession>();
  ipc::Session second = std::make_shared<ipc::ConnectionSession>();

  void SetUp() override {
    fs::create_directories(paths.normal_root);
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  }
  void TearDown() override {
    storage::internal::repository_test_hooks = {};
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  ipc::NormalMutationResponse call(ipc::MutationPayload operation, ipc::Session owner = {}) {
    const auto id = app::MutationId::generate();
    auto response =
        mutations.handle(ipc::MutationRequest{id, std::move(operation)}, owner ? owner : first);
    EXPECT_TRUE(std::holds_alternative<ipc::NormalMutationResponse>(response));
    auto result = std::get<ipc::NormalMutationResponse>(std::move(response));
    EXPECT_EQ(result.mutation_id, id);
    return result;
  }
  template <class Metadata>
  Metadata saved(ipc::MutationPayload operation, ipc::Session owner = {}) {
#ifdef _WIN32
    const auto move_id = std::visit(
        [](const auto& value) -> std::optional<core::Id> {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, app::internal::MoveMemoRequest> ||
                        std::is_same_v<T, app::internal::MoveTodoRequest>)
            return value.id;
          else
            return std::nullopt;
        },
        operation);
#endif
    auto result = call(std::move(operation), owner);
    if (auto* error = std::get_if<app::MutationFailure>(&result.result)) {
#ifdef _WIN32
      // Physical/state assertions still apply, but Windows cannot promise the
      // source-removal durability required for confirmed Move success.
      if constexpr (std::is_same_v<Metadata, core::MemoMetadata> ||
                    std::is_same_v<Metadata, core::TodoMetadata>) {
        if (move_id) {
          EXPECT_EQ(error->status, app::AuthoritativeChangeStatus::Unknown);
          EXPECT_EQ(error->evidence.steps.back().observation, app::StepObservation::Published);
          const auto saved = *catalog.read_view().find_record(*move_id);
          return std::get<storage::Located<Metadata>>(saved).metadata;
        }
      }
#endif
      ADD_FAILURE() << "Unexpected mutation failure: " << int(error->error.reason);
      throw std::runtime_error("Mutation failed");
    }
    return std::get<Metadata>(std::get<ipc::MutationValue>(std::move(result.result)));
  }
  core::MemoMetadata memo() {
    return saved<core::MemoMetadata>(
        app::internal::CreateMemoRequest{{"Memo", "body", {{"tag"}}}, time, modified, {}});
  }
  core::DirectoryMetadata directory(core::DirectoryKind kind = core::DirectoryKind::Memo) {
    return saved<core::DirectoryMetadata>(app::internal::CreateDirectoryRequest{kind, "Folder"});
  }
  app::MutationFailure failed(ipc::MutationPayload operation, ipc::Session owner = {}) {
    return std::get<app::MutationFailure>(call(std::move(operation), owner).result);
  }
  void acquire(const core::Id& id, ipc::Session owner = {}) {
    EXPECT_TRUE(std::holds_alternative<app::EditSnapshot>(
        mutations.handle(ipc::AcquireEditRequest{id}, owner ? owner : first)));
  }
};

TEST_F(MutationOrchestrator, CreateAllKindsUsesBackendIdsAndPreservesBothFrontendContexts) {
  auto a = memo(), b = memo();
  EXPECT_NE(a.id, b.id);
  auto log = saved<core::LogMetadata>(app::internal::CreateLogRequest{
      {"Log", "body", {std::chrono::year{2027} / 1 / 15, "UTC", {}}, {{"tag"}}}, time, modified});
  auto todo = saved<core::TodoMetadata>(app::internal::CreateTodoRequest{{"Task",
                                                                          "notes",
                                                                          core::TodoStatus::Pending,
                                                                          core::TodoPriority::High,
                                                                          {},
                                                                          {{"subtask", false}}},
                                                                         time,
                                                                         modified,
                                                                         {}});
  auto dir = directory();
  EXPECT_EQ(log.privacy, core::Privacy::Normal);
  EXPECT_EQ(todo.created.timestamp, time.timestamp);
  EXPECT_EQ(todo.modified.timestamp, modified.timestamp);
  EXPECT_EQ(a.modified.timezone, modified.timezone);
  EXPECT_EQ(a.modified.location, modified.location);
  EXPECT_TRUE(catalog.read_view().contains_entry(dir.id));
  EXPECT_TRUE(catalog.unpersisted());
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
}

TEST_F(MutationOrchestrator, UpdateMoveInEitherOrderPreservesCurrentMembershipAndImmutableFields) {
  auto record = memo();
  auto dir = directory();
  acquire(record.id);
  // An unrelated invalid file would break discovery. These operations use known locators.
  std::ofstream(paths.normal_root / "memos/unrelated.md") << "malformed unrelated file";
  saved<core::MemoMetadata>(app::internal::MoveMemoRequest{record.id, dir.id, modified});
  app::MemoEditableFields old_fields{"Changed", "new content", {{"new-tag"}}};
  auto updated =
      saved<core::MemoMetadata>(app::internal::UpdateMemoRequest{record.id, old_fields, time});
  EXPECT_EQ(updated.directory_id, dir.id);
  EXPECT_EQ(updated.modified.timestamp, time.timestamp); // Context is not a monotonic version.
  saved<core::MemoMetadata>(app::internal::MoveMemoRequest{record.id, {}, modified});
  const auto located = std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id));
  auto state =
      std::get<core::Memo::Snapshot>(repository.load_memo(record.id, located.relative_path));
  EXPECT_EQ(state.title, old_fields.title);
  EXPECT_EQ(state.content, old_fields.content);
  EXPECT_EQ(state.created.timestamp, record.created.timestamp);
  EXPECT_EQ(state.privacy, record.privacy);
  EXPECT_FALSE(state.directory_id);
  EXPECT_EQ(state.modified.timezone, modified.timezone);
}

TEST_F(MutationOrchestrator, LocksCoverRecordsAndDirectoriesAndExpireWithSessionOwnership) {
  auto record = memo();
  auto dir = directory();
  acquire(record.id);
  acquire(record.id); // Same owner may obtain a fresh authoritative Snapshot.
  EXPECT_EQ(
      failed(app::internal::MoveMemoRequest{record.id, dir.id, modified}, second).error.reason,
      app::ErrorReason::LockConflict);
  EXPECT_EQ(
      failed(app::internal::UpdateMemoRequest{record.id, {"title", "body", {}}, modified}, second)
          .error.reason,
      app::ErrorReason::LockConflict);
  auto release = mutations.handle(ipc::ReleaseEditRequest{record.id}, second);
  EXPECT_EQ(std::get<app::ApplicationError>(release).reason, app::ErrorReason::LockConflict);
  acquire(dir.id);
  EXPECT_EQ(failed(app::internal::RenameDirectoryRequest{dir.id, "Other"}, second).error.reason,
            app::ErrorReason::LockConflict);
  saved<core::DirectoryMetadata>(app::internal::RenameDirectoryRequest{dir.id, "Owner rename"});
  EXPECT_TRUE(std::holds_alternative<core::Memo::Snapshot>(repository.load_memo(
      record.id,
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).relative_path)));
  first.reset();
  acquire(record.id, second);
  acquire(dir.id, second);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::ReleaseEditRequest{record.id}, second)));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::ReleaseEditRequest{record.id}, second)));
}

TEST_F(MutationOrchestrator, ImmediateMutationsDoNotRequireLongLivedLocksButContentUpdatesDo) {
  auto record = memo();
  EXPECT_EQ(failed(app::internal::UpdateMemoRequest{record.id, {"title", "body", {}}, modified})
                .error.reason,
            app::ErrorReason::EditLockRequired);
  auto dir = directory();
  saved<core::MemoMetadata>(app::internal::MoveMemoRequest{record.id, dir.id, modified});
  saved<core::DirectoryMetadata>(app::internal::RenameDirectoryRequest{dir.id, "Renamed"});
  auto task = saved<core::TodoMetadata>(app::internal::CreateTodoRequest{{"Task",
                                                                          "retain notes",
                                                                          core::TodoStatus::Pending,
                                                                          core::TodoPriority::High,
                                                                          {},
                                                                          {{"retain", true}}},
                                                                         time,
                                                                         modified,
                                                                         {}});
  saved<core::TodoMetadata>(
      app::internal::SetTodoStatusRequest{task.id, core::TodoStatus::Completed, time});
  const auto locator =
      std::get<storage::TodoMetadata>(*catalog.read_view().find_record(task.id)).relative_path;
  auto state = std::get<core::Todo::Snapshot>(repository.load_todo(task.id, locator));
  EXPECT_EQ(state.notes, "retain notes");
  ASSERT_EQ(state.subtasks.size(), 1u);
  EXPECT_TRUE(state.subtasks[0].completed);
  acquire(task.id);
  EXPECT_EQ(
      failed(app::internal::SetTodoStatusRequest{task.id, core::TodoStatus::Pending, modified},
             second)
          .error.reason,
      app::ErrorReason::LockConflict);
}

TEST_F(MutationOrchestrator, PrerequisitesAndFailedAcquisitionDoNotPublishOrHoldLocks) {
  auto record = memo();
  EXPECT_EQ(failed(app::internal::UpdateMemoRequest{
                       core::Id::generate(), {"title", "body", {}}, modified})
                .error.reason,
            app::ErrorReason::NotFound);
  EXPECT_EQ(
      failed(app::internal::UpdateTodoRequest{
                 record.id,
                 {"title", "notes", core::TodoStatus::Pending, core::TodoPriority::None, {}, {}},
                 modified})
          .error.reason,
      app::ErrorReason::WrongRecordType);
  auto wrong_dir = directory(core::DirectoryKind::Todo);
  auto error = failed(app::internal::MoveMemoRequest{record.id, wrong_dir.id, modified});
  EXPECT_EQ(error.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(error.error.reason, app::ErrorReason::WrongRecordType);
  auto locator =
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).relative_path;
  fs::remove(paths.normal_root / locator);
  auto acquired = mutations.handle(ipc::AcquireEditRequest{record.id}, first);
  EXPECT_EQ(std::get<app::ApplicationError>(acquired).reason, app::ErrorReason::StaleLocation);
  auto stale = failed(app::internal::MoveMemoRequest{record.id, {}, modified}, second);
  EXPECT_EQ(stale.error.reason, app::ErrorReason::StaleLocation);
  EXPECT_EQ(stale.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(MutationOrchestrator, BeforePublicationAndPublishedUncertainFailuresRemainDistinct) {
  auto record = memo();
  acquire(record.id);
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view, bool) -> storage::StorageResult<std::monostate> {
    return folio::io::FileError{folio::io::FileErrorCode::IoError, path, "injected", {}, false};
  };
  auto before =
      failed(app::internal::UpdateMemoRequest{record.id, {"title", "body", {}}, modified});
  EXPECT_EQ(before.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(before.evidence.steps[0].observation, app::StepObservation::NotCompleted);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view bytes,
         bool) -> storage::StorageResult<std::monostate> {
    auto written = folio::io::replace_file_atomic(path, bytes);
    if (auto* failure = std::get_if<folio::io::FileError>(&written)) return *failure;
    return folio::io::FileError{
        folio::io::FileErrorCode::DurabilityFailure, path, "injected", {}, true};
  };
  auto after =
      failed(app::internal::UpdateMemoRequest{record.id, {"title", "written", {}}, modified});
  EXPECT_EQ(after.status, app::AuthoritativeChangeStatus::Unknown);
  EXPECT_EQ(after.evidence.steps[0].observation, app::StepObservation::Published);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.finish_session()));
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MutationOrchestrator, CatalogFailureRetainsAppliedAndPreventsFurtherWrites) {
  auto record = memo();
  acquire(record.id);
  storage::internal::repository_test_hooks.before_projection = [] { throw 7; };
  auto failure = failed(
      app::internal::UpdateMemoRequest{record.id, {"new title", "saved content", {}}, modified});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::Applied);
  EXPECT_EQ(failure.error.reason, app::ErrorReason::CatalogSynchronizationFailed);
  EXPECT_EQ(failure.evidence.steps[0].observation, app::StepObservation::DurabilityConfirmed);
  storage::internal::repository_test_hooks = {};
  auto locator =
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).relative_path;
  EXPECT_EQ(std::get<core::Memo::Snapshot>(repository.load_memo(record.id, locator)).content,
            "saved content");
  EXPECT_EQ(
      failed(app::internal::UpdateMemoRequest{record.id, {"later", "must not save", {}}, modified})
          .status,
      app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(coordinator.finish_session()));
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MutationOrchestrator, MoveFailureRetainsDurableDestinationAndUnremovedSource) {
  auto record = memo();
  auto dir = directory();
  auto source =
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).relative_path;
  storage::internal::repository_test_hooks.remove_source = [](const fs::path&,
                                                              std::error_code& ec) {
    ec = std::make_error_code(std::errc::permission_denied);
    return false;
  };
  auto failure = failed(app::internal::MoveMemoRequest{record.id, dir.id, modified});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::PartiallyApplied);
  ASSERT_EQ(failure.evidence.steps.size(), 2u);
  EXPECT_EQ(failure.evidence.steps[0].observation, app::StepObservation::DurabilityConfirmed);
  EXPECT_EQ(failure.evidence.steps[1].observation, app::StepObservation::NotCompleted);
  EXPECT_TRUE(fs::exists(paths.normal_root / source));
  EXPECT_TRUE(fs::exists(paths.normal_root / "memos" / dir.id.to_string() / source.filename()));
  EXPECT_EQ(
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).relative_path,
      source);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
}

#ifndef _WIN32
TEST_F(MutationOrchestrator, MoveRemovalSyncFailureIsUnknownRatherThanApplied) {
  auto record = memo();
  auto dir = directory();
  storage::internal::repository_test_hooks.sync_source = [](int) {
    errno = EIO;
    return false;
  };
  auto failure = failed(app::internal::MoveMemoRequest{record.id, dir.id, modified});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::Unknown);
  EXPECT_EQ(failure.evidence.steps[1].observation, app::StepObservation::Published);
}
#endif

TEST_F(MutationOrchestrator, DirectoryCreationFailureDoesNotCallEmptyFolderAnAppliedEntry) {
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view, bool) -> storage::StorageResult<std::monostate> {
    return folio::io::FileError{folio::io::FileErrorCode::IoError, path, "injected", {}, false};
  };
  auto failure = failed(app::internal::CreateDirectoryRequest{core::DirectoryKind::Memo, "Folder"});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_TRUE(
      fs::is_directory(paths.normal_root / "memos" / failure.evidence.entry_id.to_string()));
  EXPECT_FALSE(catalog.read_view().contains_entry(failure.evidence.entry_id));
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MutationOrchestrator, MutationAndLockWireTypesRoundTripUsingExistingProtocol) {
  auto record = memo();
  auto dir = directory();
  std::vector<ipc::OperationRequest> requests{
      ipc::AcquireEditRequest{record.id}, ipc::ReleaseEditRequest{record.id},
      ipc::MutationRequest{
          app::MutationId::generate(),
          app::internal::UpdateMemoRequest{record.id, {"title", "body", {{"tag"}}}, modified}},
      ipc::MutationRequest{app::MutationId::generate(),
                           app::internal::MoveMemoRequest{record.id, dir.id, modified}}};
  for (const auto& request : requests) {
    auto bytes = std::get<std::string>(ipc::codec::encode<ipc::OperationRequest>(request));
    auto decoded = ipc::codec::decode<ipc::OperationRequest>(bytes);
    ASSERT_TRUE(std::holds_alternative<ipc::OperationRequest>(decoded));
    EXPECT_EQ(std::get<std::string>(ipc::codec::encode<ipc::OperationRequest>(
                  std::get<ipc::OperationRequest>(decoded))),
              bytes);
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
        ipc::codec::decode<ipc::OperationRequest>(bytes + "x")));
  }
}
} // namespace

TEST_F(MutationOrchestrator,
       AcceptedActionKeepsOwnershipAfterDisconnectAndCleanupFollowsExecution) {
  auto record = memo();
  acquire(record.id);
  ipc::RequestScheduler scheduler(4);
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  auto blocking = scheduler.admit([&]() -> ipc::RequestScheduler::Result {
    entered.set_value();
    gate.wait();
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  entered.get_future().wait();
  std::optional<app::MutationFailure> other_failure;
  auto other = scheduler.admit([&]() -> ipc::RequestScheduler::Result {
    other_failure = failed(app::internal::MoveMemoRequest{record.id, {}, modified}, second);
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  bool saved_after_disconnect = false;
  auto accepted = scheduler.admit([&, owner = first]() -> ipc::RequestScheduler::Result {
    saved<core::MemoMetadata>(
        app::internal::UpdateMemoRequest{record.id, {"saved", "accepted body", {}}, modified},
        owner);
    saved_after_disconnect = true;
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  first.reset(); // Simulate worker exit; the accepted action still owns the session.
  release.set_value();
  scheduler.drain();
  ASSERT_TRUE(other_failure);
  EXPECT_EQ(other_failure->error.reason, app::ErrorReason::LockConflict);
  EXPECT_TRUE(saved_after_disconnect);
  acquire(record.id, second); // Accepted work settled; stale ownership is gone.
}

TEST_F(MutationOrchestrator,
       InvalidFieldsAreRejectedBeforePublicationAndSessionFinalizationBlocksWrites) {
  auto bad = failed(app::internal::CreateMemoRequest{
      {std::string(1, char(0xff)), "body", {}}, time, modified, {}});
  EXPECT_EQ(bad.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(bad.error.reason, app::ErrorReason::InvalidMutation);
  EXPECT_FALSE(catalog.read_view().contains_entry(bad.evidence.entry_id));
  EXPECT_FALSE(fs::exists(paths.normal_root / "memos"));
  auto record = memo();
  acquire(record.id);
  ASSERT_TRUE(std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  auto late = failed(
      app::internal::UpdateMemoRequest{record.id, {"late", "must not publish", {}}, modified});
  EXPECT_EQ(late.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(late.error.reason, app::ErrorReason::MetadataUnavailable);
  EXPECT_FALSE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MutationOrchestrator, DurableWriteWithCleanupFailureIsAppliedAndKeepsMarker) {
  storage::internal::repository_test_hooks.write =
      [](const fs::path& path, std::string_view bytes,
         bool) -> storage::StorageResult<std::monostate> {
    auto written = folio::io::create_file_atomic(path, bytes);
    if (auto* failure = std::get_if<folio::io::FileError>(&written)) return *failure;
    return folio::io::FileError{folio::io::FileErrorCode::DurabilityFailure,
                                path,
                                "injected cleanup failure",
                                {},
                                true,
                                true};
  };
  auto failure =
      failed(app::internal::CreateMemoRequest{{"title", "durable body", {}}, time, modified, {}});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::Applied);
  EXPECT_EQ(failure.evidence.steps[0].observation, app::StepObservation::DurabilityConfirmed);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Inconsistent);
  EXPECT_TRUE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MutationOrchestrator, IpcDisconnectPreservesQueuedOwnerUpdateAndExcludesCompetingMove) {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  auto record = memo();
  paths.application.runtime = base / "runtime";
  auto ownership_result = ipc::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<ipc::RepositoryOwnership>(ownership_result));
  auto ownership = std::get<ipc::RepositoryOwnership>(std::move(ownership_result));
  const auto blocked_id = core::Id::generate();
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::stop_source shutdown;
  bool released = false;
  std::exception_ptr server_exception;
  ipc::IpcServer server(ownership,
                        [&](const ipc::OperationRequest& request,
                            const ipc::Session& session) -> ipc::OperationResponse {
                          if (const auto* load = std::get_if<ipc::LoadMemoRequest>(&request);
                              load && load->id == blocked_id) {
                            entered.set_value();
                            gate.wait();
                          }
                          if (std::holds_alternative<ipc::MutationRequest>(request) ||
                              std::holds_alternative<ipc::AcquireEditRequest>(request) ||
                              std::holds_alternative<ipc::ReleaseEditRequest>(request))
                            return mutations.handle(request, session);
                          return ipc::AccessOrchestrator{catalog, repository, coordinator}.handle(
                              request);
                        });
  std::thread thread([&] {
    try {
      auto result = server.run(shutdown.get_token());
      EXPECT_TRUE(std::holds_alternative<std::monostate>(result));
      server.drain_requests();
      server.close_connections();
    } catch (...) {
      server_exception = std::current_exception();
    }
  });
  struct Cleanup {
    std::function<void()> action;
    ~Cleanup() { action(); }
  } cleanup{[&] {
    if (!released) release.set_value();
    shutdown.request_stop();
    if (thread.joinable()) thread.join();
    EXPECT_FALSE(server_exception);
  }};
  const auto settings = std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths));
  auto receive = [&](ipc::IpcStream& stream) {
    ipc::MessageTransport transport;
    auto message = ipc::receive_message(stream, transport, Clock::now() + 3s);
    if (auto* result = std::get_if<ipc::Message>(&message)) return std::move(*result);
    throw std::runtime_error("Missing test response");
  };
  auto send = [&](ipc::IpcStream& stream, const ipc::OperationRequest& request) {
    auto bytes = std::get<std::string>(ipc::codec::encode<ipc::OperationRequest>(request));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(ipc::send_message(
        stream, {ipc::MessageKind::Request, ipc::payload_bytes(bytes)}, Clock::now() + 3s)));
  };
  auto connect = [&] {
    auto stream = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings.endpoint));
    auto hello = ipc::exchange_message(stream, {ipc::MessageKind::Hello, {}}, Clock::now() + 3s);
    EXPECT_EQ(std::get<ipc::Message>(hello).kind, ipc::MessageKind::Ready);
    return stream;
  };
  auto owner = connect(), blocker = connect(), competitor = connect();
  send(owner, ipc::AcquireEditRequest{record.id});
  EXPECT_EQ(receive(owner).kind, ipc::MessageKind::Accepted);
  EXPECT_TRUE(std::holds_alternative<app::EditSnapshot>(std::get<ipc::OperationResponse>(
      ipc::codec::decode<ipc::OperationResponse>(ipc::payload_text(receive(owner).payload)))));
  send(blocker, ipc::LoadMemoRequest{blocked_id});
  EXPECT_EQ(receive(blocker).kind, ipc::MessageKind::Accepted);
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  // This competing request executes first after the blocker, but may not
  // bypass ownership retained by the owner's already accepted queued Update.
  send(competitor, ipc::MutationRequest{app::MutationId::generate(),
                                        app::internal::MoveMemoRequest{record.id, {}, modified}});
  EXPECT_EQ(receive(competitor).kind, ipc::MessageKind::Accepted);
  send(owner, ipc::MutationRequest{app::MutationId::generate(),
                                   app::internal::UpdateMemoRequest{
                                       record.id, {"saved", "after disconnect", {}}, modified}});
  EXPECT_EQ(receive(owner).kind, ipc::MessageKind::Accepted);
  owner = ipc::IpcStream{};
  release.set_value();
  released = true;
  EXPECT_EQ(receive(blocker).kind, ipc::MessageKind::Response);
  const auto conflict = std::get<ipc::NormalMutationResponse>(std::get<ipc::OperationResponse>(
      ipc::codec::decode<ipc::OperationResponse>(ipc::payload_text(receive(competitor).payload))));
  EXPECT_EQ(std::get<app::MutationFailure>(conflict.result).error.reason,
            app::ErrorReason::LockConflict);
  send(competitor, ipc::LoadMemoRequest{record.id});
  EXPECT_EQ(receive(competitor).kind, ipc::MessageKind::Accepted);
  const auto loaded = std::get<core::Memo::Snapshot>(std::get<ipc::OperationResponse>(
      ipc::codec::decode<ipc::OperationResponse>(ipc::payload_text(receive(competitor).payload))));
  EXPECT_EQ(loaded.content, "after disconnect");
  send(competitor, ipc::AcquireEditRequest{record.id});
  EXPECT_EQ(receive(competitor).kind, ipc::MessageKind::Accepted);
  EXPECT_TRUE(std::holds_alternative<app::EditSnapshot>(std::get<ipc::OperationResponse>(
      ipc::codec::decode<ipc::OperationResponse>(ipc::payload_text(receive(competitor).payload)))));
}

TEST_F(MutationOrchestrator, LogAndTodoFullEditableFieldsReplaceOnlyTheirSupportedState) {
  auto log = saved<core::LogMetadata>(app::internal::CreateLogRequest{
      {"Log", "old", {std::chrono::year{2027} / 1 / 15, "UTC", {}}, {{"old"}}}, time, time});
  acquire(log.id);
  const core::DateContext new_date{std::chrono::year{2027} / 2 / 3, "Asia/Shanghai",
                                   "date context"};
  auto updated_log = saved<core::LogMetadata>(
      app::internal::UpdateLogRequest{log.id, {"New log", "new body", new_date, {}}, modified});
  EXPECT_EQ(updated_log.log_date.date, new_date.date);
  EXPECT_TRUE(updated_log.tags.empty());
  const auto log_locator =
      std::get<storage::LogMetadata>(*catalog.read_view().find_record(log.id)).relative_path;
  const auto log_state = std::get<core::Log::Snapshot>(repository.load_log(log.id, log_locator));
  EXPECT_EQ(log_state.content, "new body");
  EXPECT_EQ(log_state.created.timestamp, time.timestamp);
  auto todo = saved<core::TodoMetadata>(app::internal::CreateTodoRequest{{"Task",
                                                                          "old notes",
                                                                          core::TodoStatus::Pending,
                                                                          core::TodoPriority::High,
                                                                          {},
                                                                          {{"old subtask", false}}},
                                                                         time,
                                                                         time,
                                                                         {}});
  acquire(todo.id);
  const core::DueDate due = new_date;
  auto updated =
      saved<core::TodoMetadata>(app::internal::UpdateTodoRequest{todo.id,
                                                                 {"New task",
                                                                  "new notes",
                                                                  core::TodoStatus::Completed,
                                                                  core::TodoPriority::Low,
                                                                  due,
                                                                  {{"new subtask", true}}},
                                                                 modified});
  EXPECT_EQ(updated.status, core::TodoStatus::Completed);
  EXPECT_EQ(updated.priority, core::TodoPriority::Low);
  EXPECT_TRUE(updated.due_date);
  const auto todo_locator =
      std::get<storage::TodoMetadata>(*catalog.read_view().find_record(todo.id)).relative_path;
  const auto todo_state =
      std::get<core::Todo::Snapshot>(repository.load_todo(todo.id, todo_locator));
  EXPECT_EQ(todo_state.notes, "new notes");
  ASSERT_EQ(todo_state.subtasks.size(), 1u);
  EXPECT_EQ(todo_state.subtasks[0].content, "new subtask");
  EXPECT_TRUE(todo_state.subtasks[0].completed);
  EXPECT_EQ(todo_state.modified.location, modified.location);
}

TEST_F(MutationOrchestrator, AuthoritativeMetadataDisagreementRejectsWithoutImplicitRepair) {
  auto record = memo();
  acquire(record.id);
  const auto located = std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id));
  auto external =
      std::get<core::Memo::Snapshot>(repository.load_memo(record.id, located.relative_path));
  external.title = "externally changed";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.update_memo(external, located.relative_path)));
  auto failure =
      failed(app::internal::UpdateMemoRequest{record.id, {"edit", "body", {}}, modified});
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::NotApplied);
  EXPECT_EQ(failure.error.reason, app::ErrorReason::InvalidData);
  EXPECT_EQ(
      std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id)).metadata.title,
      record.title);
  EXPECT_EQ(
      std::get<core::Memo::Snapshot>(repository.load_memo(record.id, located.relative_path)).title,
      external.title);
  EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
}

TEST_F(MutationOrchestrator, UnifiedMoveResolvesTypeAndRejectsIncompatibleTargets) {
  auto record = memo();
  auto folder = directory();
  auto wrong = directory(core::DirectoryKind::Todo);
  auto rejected = failed(ipc::MoveRecordRequest{record.id, wrong.id, modified});
  EXPECT_EQ(rejected.status, app::AuthoritativeChangeStatus::NotApplied);
  auto wire = ipc::OperationRequest{ipc::MutationRequest{
      app::MutationId::generate(), ipc::MoveRecordRequest{record.id, folder.id, modified}}};
  EXPECT_TRUE(ipc::codec::fits_message(wire));
  auto moved = call(ipc::MoveRecordRequest{record.id, folder.id, modified});
#ifndef _WIN32
  ASSERT_TRUE(std::holds_alternative<ipc::MutationValue>(moved.result));
#endif
  EXPECT_EQ(std::get<storage::MemoMetadata>(*catalog.read_view().find_record(record.id))
                .metadata.directory_id,
            folder.id);
  auto denied = failed(ipc::MoveRecordRequest{folder.id, {}, modified});
  EXPECT_EQ(denied.error.reason, app::ErrorReason::WrongRecordType);
  EXPECT_EQ(denied.status, app::AuthoritativeChangeStatus::NotApplied);
}
