#include "execution/request_coordinator.hpp"
#include "ipc/ipc_server.hpp"
#include "backend_connection_handle.hpp"
#include "operations/access_orchestrator.hpp"
#include "operations/mutation_orchestrator.hpp"
#include "storage/normal_repository_test_hooks.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <fstream>

namespace {
namespace ipc = folio::application::internal;
namespace app = folio::application;
namespace core = folio::core;
namespace storage = core::storage;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Lane = ipc::RequestCoordinator::Lane;
using Ticket = std::shared_ptr<ipc::RequestCoordinator::Ticket>;

// Release even after an early assertion, before the coordinator is destroyed.
struct Gate {
  std::promise<void> release;
  std::shared_future<void> ready = release.get_future().share();
  bool opened = false;
  void open() {
    if (!opened) {
      opened = true;
      release.set_value();
    }
  }
  ~Gate() { open(); }
};
ipc::RequestCoordinator::Result empty_result() {
  return ipc::Message{ipc::MessageKind::Response, {}};
}
bool finished(const Ticket& ticket) {
  std::promise<void> ready;
  auto future = ready.get_future();
  std::stop_callback signal(ticket->completed.get_token(), [&] { ready.set_value(); });
  return future.wait_for(2s) == std::future_status::ready;
}

TEST(ConcurrentExecution, SlowMutationDoesNotBlockControlQueryOrUnrelatedLoads) {
  ipc::RequestCoordinator coordinator(12, 2);
  Gate hold;
  std::promise<void> writing;
  std::thread::id coordination_thread, writer_thread, query_thread, load_thread;
  const auto caller = std::this_thread::get_id();
  auto mutation = coordinator.admit({Lane::Mutation, "A"}, [&] {
    coordination_thread = std::this_thread::get_id();
    return [&, wait = hold.ready] {
      writer_thread = std::this_thread::get_id();
      writing.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(writing.get_future().wait_for(2s), std::future_status::ready);
  auto control = coordinator.admit({Lane::Control, "A"}, [&] {
    EXPECT_EQ(std::this_thread::get_id(), coordination_thread);
    return [&] {
      EXPECT_EQ(std::this_thread::get_id(), coordination_thread);
      return empty_result();
    };
  });
  auto query = coordinator.admit({Lane::Query, {}}, [&] {
    EXPECT_NE(std::this_thread::get_id(), coordination_thread);
    return [&] {
      query_thread = std::this_thread::get_id();
      return empty_result();
    };
  });
  auto load = coordinator.admit({Lane::Load, "B"}, [&] {
    return [&] {
      load_thread = std::this_thread::get_id();
      return empty_result();
    };
  });
  EXPECT_TRUE(finished(control));
  EXPECT_TRUE(finished(query));
  EXPECT_TRUE(finished(load));
  EXPECT_NE(coordination_thread, caller);
  EXPECT_NE(query_thread, coordination_thread);
  EXPECT_NE(load_thread, coordination_thread);
  EXPECT_NE(writer_thread, coordination_thread);
  EXPECT_NE(query_thread, writer_thread);
  EXPECT_NE(load_thread, writer_thread);
  hold.open();
  coordinator.drain();
  EXPECT_TRUE(finished(mutation));
}

TEST(ConcurrentExecution, WaitingWriterPreventsLaterLoadsFromOvertakingAndCapturesCurrentLocation) {
  ipc::RequestCoordinator coordinator(10, 2);
  Gate reading, writing;
  std::promise<void> reader_started, writer_started;
  std::string location = "old";
  std::atomic<bool> later_prepared = false;
  auto first = coordinator.admit({Lane::Load, "A"}, [&] {
    EXPECT_EQ(location, "old");
    return [&, wait = reading.ready] {
      reader_started.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(reader_started.get_future().wait_for(2s), std::future_status::ready);
  auto writer = coordinator.admit({Lane::Mutation, "A"}, [&] {
    return [&, wait = writing.ready] {
      writer_started.set_value();
      wait.wait();
      location = "new";
      return empty_result();
    };
  });
  auto later = coordinator.admit({Lane::Load, "A"}, [&] {
    later_prepared = true;
    EXPECT_EQ(location, "new");
    return [] { return empty_result(); };
  });
  auto other = coordinator.admit({Lane::Load, "B"}, [] { return [] { return empty_result(); }; });
  EXPECT_TRUE(finished(other));
  EXPECT_FALSE(later_prepared.load());
  reading.open();
  ASSERT_EQ(writer_started.get_future().wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(later_prepared.load());
  writing.open();
  coordinator.drain();
  EXPECT_TRUE(finished(first));
  EXPECT_TRUE(finished(writer));
  EXPECT_TRUE(finished(later));
}

TEST(ConcurrentExecution, ConfiguredLoadCapacityIsBoundedWhileQueryAndControlProgress) {
  for (std::size_t workers : {1u, 3u}) {
    ipc::RequestCoordinator coordinator(10, workers);
    Gate hold;
    std::atomic<std::size_t> started = 0;
    std::promise<void> full;
    std::vector<Ticket> tickets;
    for (std::size_t i = 0; i < workers + 1; ++i) {
      tickets.push_back(coordinator.admit({Lane::Load, std::to_string(i)}, [&] {
        return [&, wait = hold.ready] {
          if (++started == workers) full.set_value();
          wait.wait();
          return empty_result();
        };
      }));
    }
    ASSERT_EQ(full.get_future().wait_for(2s), std::future_status::ready);
    auto control =
        coordinator.admit({Lane::Control, {}}, [] { return [] { return empty_result(); }; });
    EXPECT_TRUE(finished(control));
    EXPECT_EQ(started.load(), workers);
    coordinator.stop_admission();
    EXPECT_FALSE(coordinator.admit([] { return empty_result(); }));
    hold.open();
    coordinator.drain();
    EXPECT_EQ(started.load(), workers + 1);
    for (const auto& ticket : tickets)
      EXPECT_TRUE(finished(ticket));
  }
  EXPECT_THROW(ipc::RequestCoordinator(1, 0), std::invalid_argument);
  EXPECT_NO_THROW(ipc::RequestCoordinator(1, 17));
}

TEST(ConcurrentExecution, PreparationAndExecutionExceptionsReleaseReservationsAndDrainAllLanes) {
  ipc::RequestCoordinator coordinator(8);
  auto bad_prepare =
      coordinator.admit({Lane::Mutation, "A"}, []() -> ipc::RequestCoordinator::Action {
        throw std::runtime_error("synthetic preparation failure");
      });
  auto bad_execute = coordinator.admit({Lane::Load, "A"}, [] {
    return []() -> ipc::RequestCoordinator::Result {
      throw std::runtime_error("synthetic read failure");
    };
  });
  auto next =
      coordinator.admit({Lane::Mutation, "A"}, [] { return [] { return empty_result(); }; });
  auto query = coordinator.admit({Lane::Query, {}}, [] { return [] { return empty_result(); }; });
  coordinator.drain();
  EXPECT_TRUE(bad_prepare->exception);
  EXPECT_TRUE(bad_execute->exception);
  EXPECT_FALSE(next->exception);
  EXPECT_TRUE(finished(next));
  EXPECT_TRUE(finished(query));
}

TEST(ConcurrentExecution, QueriesAndLoadsShareCapacityAndPrepareOnTheirExecutingWorker) {
  ipc::RequestCoordinator coordinator(8, 1);
  Gate hold;
  std::promise<void> entered;
  std::thread::id load_thread;
  std::atomic<bool> query_prepared = false;
  auto load = coordinator.admit({Lane::Load, "A"}, [&] {
    load_thread = std::this_thread::get_id();
    return [&, wait = hold.ready] {
      EXPECT_EQ(std::this_thread::get_id(), load_thread);
      entered.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  auto query = coordinator.admit({Lane::Query, {}}, [&] {
    query_prepared = true;
    EXPECT_EQ(std::this_thread::get_id(), load_thread);
    return [&] {
      EXPECT_EQ(std::this_thread::get_id(), load_thread);
      return empty_result();
    };
  });
  auto control =
      coordinator.admit({Lane::Control, {}}, [] { return [] { return empty_result(); }; });
  EXPECT_TRUE(finished(control));
  EXPECT_FALSE(query_prepared.load());
  hold.open();
  coordinator.drain();
  EXPECT_TRUE(finished(load));
  EXPECT_TRUE(finished(query));
  EXPECT_TRUE(query_prepared.load());
}

TEST(ConcurrentExecution, MultipleQueriesRunConcurrentlyInSharedAccessPool) {
  ipc::RequestCoordinator coordinator(8, 2);
  Gate hold;
  std::promise<void> first_started, second_started;
  auto first = coordinator.admit({Lane::Query, {}}, [&] {
    return [&, wait = hold.ready] {
      first_started.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(first_started.get_future().wait_for(2s), std::future_status::ready);
  auto second = coordinator.admit({Lane::Query, {}}, [&] {
    return [&, wait = hold.ready] {
      second_started.set_value();
      wait.wait();
      return empty_result();
    };
  });
  EXPECT_EQ(second_started.get_future().wait_for(2s), std::future_status::ready);
  hold.open();
  coordinator.drain();
  EXPECT_TRUE(finished(first));
  EXPECT_TRUE(finished(second));
}

class ConcurrentOperations : public testing::Test {
protected:
  fs::path base = fs::weakly_canonical(fs::temp_directory_path()) /
                  ("fconc-" + core::Id::generate().to_string().substr(0, 8));
  storage::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state", base / "cache",
                               base / "logs", base / "runtime"}};
  storage::MetadataCatalog catalog;
  storage::NormalRepository repository{paths.normal_root};
  storage::CatalogCoordinator catalog_coordinator{paths, catalog};
  ipc::AccessOrchestrator access{catalog, repository, catalog_coordinator};
  ipc::MutationOrchestrator mutations{catalog, repository, catalog_coordinator};
  ipc::Session owner = std::make_shared<ipc::ConnectionSession>();
  ipc::Session other = std::make_shared<ipc::ConnectionSession>();
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", {}};
  core::Memo::Snapshot memo = core::Memo{"original", "body", time}.snapshot();
  void SetUp() override {
    fs::create_directories(paths.normal_root);
    ASSERT_TRUE(
        std::holds_alternative<storage::InitializationOutcome>(catalog_coordinator.startup_load()));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(
        memo, catalog_coordinator.memo_creation_callback(), catalog_coordinator.creation_check())));
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  ipc::OperationResponse mutate(ipc::MutationPayload payload) {
    return mutations.handle(ipc::MutationRequest{app::MutationId::generate(), std::move(payload)},
                            owner);
  }
  Ticket submit(ipc::RequestCoordinator& coordinator, ipc::OperationRequest request,
                ipc::OperationResponse& result, ipc::Session session = {}) {
    auto plan = ipc::RequestCoordinator::plan(request);
    return coordinator.admit(
        plan, [&, request = std::move(request), session = session ? session : owner]() mutable {
          auto run = std::holds_alternative<ipc::MutationRequest>(request) ||
                             std::holds_alternative<ipc::AcquireEditRequest>(request) ||
                             std::holds_alternative<ipc::ReleaseEditRequest>(request)
                         ? mutations.prepare(std::move(request), session)
                         : access.prepare(std::move(request));
          return [&, run = std::move(run)] {
            result = run();
            return empty_result();
          };
        });
  }
};

TEST_F(ConcurrentOperations, PreparedQueryUsesOneOldViewWhileNewQuerySeesPublishedUpdate) {
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::AcquireEditRequest{memo.id}, owner)));
  ipc::RequestCoordinator coordinator(8);
  Gate hold;
  std::promise<void> captured;
  ipc::OperationResponse old_result, changed, fresh;
  auto old = coordinator.admit({Lane::Query, {}}, [&] {
    auto query = access.prepare(ipc::BrowseRequest{core::RecordType::Memo, {}});
    captured.set_value();
    return [&, query, wait = hold.ready] {
      wait.wait();
      old_result = query();
      return empty_result();
    };
  });
  ASSERT_EQ(captured.get_future().wait_for(2s), std::future_status::ready);
  auto update = submit(
      coordinator,
      ipc::MutationRequest{app::MutationId::generate(),
                           ipc::UpdateMemoRequest{memo.id, {"changed", "new body", {}}, time}},
      changed);
  ASSERT_TRUE(finished(update));
  auto response = std::get<ipc::NormalMutationResponse>(changed);
  EXPECT_TRUE(std::holds_alternative<ipc::MutationValue>(response.result));
  hold.open();
  ASSERT_TRUE(finished(old));
  auto next = submit(coordinator, ipc::BrowseRequest{core::RecordType::Memo, {}}, fresh);
  EXPECT_TRUE(finished(next));
  coordinator.drain();
  EXPECT_EQ(
      std::get<core::MemoMetadata>(
          std::get<app::ApplicationResponse<app::BrowseResult>>(old_result).value.records.at(0))
          .title,
      "original");
  EXPECT_EQ(std::get<core::MemoMetadata>(
                std::get<app::ApplicationResponse<app::BrowseResult>>(fresh).value.records.at(0))
                .title,
            "changed");
}

TEST_F(ConcurrentOperations, LoadMoveLoadUsesFreshLocatorWithoutDiscoveryOrBusy) {
  auto created = std::get<ipc::NormalMutationResponse>(
      mutate(ipc::CreateDirectoryRequest{core::DirectoryKind::Memo, "destination"}));
  auto directory = std::get<core::DirectoryMetadata>(std::get<ipc::MutationValue>(created.result));
  ipc::RequestCoordinator coordinator(8);
  Gate hold;
  std::promise<void> loaded;
  ipc::OperationResponse before, moved, after;
  auto first = coordinator.admit({Lane::Load, memo.id.to_string()}, [&] {
    auto action = access.prepare(ipc::LoadMemoRequest{memo.id});
    return [&, action, wait = hold.ready] {
      before = action();
      loaded.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(loaded.get_future().wait_for(2s), std::future_status::ready);
  auto move = submit(coordinator,
                     ipc::MutationRequest{app::MutationId::generate(),
                                          ipc::MoveMemoRequest{memo.id, directory.id, time}},
                     moved);
  auto last = submit(coordinator, ipc::LoadMemoRequest{memo.id}, after);
  hold.open();
  coordinator.drain();
  EXPECT_TRUE(finished(first));
  EXPECT_TRUE(finished(move));
  EXPECT_TRUE(finished(last));
  EXPECT_FALSE(std::get<core::Memo::Snapshot>(before).directory_id);
  EXPECT_EQ(std::get<core::Memo::Snapshot>(after).directory_id, directory.id);
  EXPECT_EQ(std::get<core::Memo::Snapshot>(after).content, memo.content);
}

TEST_F(ConcurrentOperations, ImmediateMutationProtectsOwnershipButDoesNotBlockOtherEntryControl) {
  ipc::RequestCoordinator coordinator(8);
  Gate hold;
  std::promise<void> protected_entry;
  ipc::OperationResponse mutated, acquisition;
  auto work = coordinator.admit({Lane::Mutation, memo.id.to_string()}, [&] {
    auto run = mutations.prepare(
        ipc::MutationRequest{app::MutationId::generate(), ipc::MoveMemoRequest{memo.id, {}, time}},
        owner);
    protected_entry.set_value();
    return [&, run, wait = hold.ready] {
      wait.wait();
      mutated = run();
      return empty_result();
    };
  });
  ASSERT_EQ(protected_entry.get_future().wait_for(2s), std::future_status::ready);
  auto acquire = submit(coordinator, ipc::AcquireEditRequest{memo.id}, acquisition, other);
  ASSERT_TRUE(finished(acquire));
  EXPECT_EQ(std::get<app::ApplicationError>(acquisition).reason, app::ErrorReason::LockConflict);
  hold.open();
  EXPECT_TRUE(finished(work));
  auto retry = submit(coordinator, ipc::AcquireEditRequest{memo.id}, acquisition, other);
  EXPECT_TRUE(finished(retry));
  coordinator.drain();
  EXPECT_TRUE(std::holds_alternative<std::monostate>(acquisition));
}

TEST_F(ConcurrentOperations,
       PublicationFailureLeavesOldViewWithInconsistentStatusAndNoFalseSuccess) {
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::AcquireEditRequest{memo.id}, owner)));
  const auto old = catalog_coordinator.read_state().view;
  ipc::RequestCoordinator coordinator(8);
  ipc::OperationResponse failed, queried;
  auto update = coordinator.admit({Lane::Mutation, memo.id.to_string()}, [&] {
    auto run = mutations.prepare(
        ipc::MutationRequest{app::MutationId::generate(),
                             ipc::UpdateMemoRequest{memo.id, {"changed", "body", {}}, time}},
        owner);
    return [&, run] {
      storage::internal::repository_test_hooks.before_projection = [] {
        throw std::runtime_error("synthetic projection failure");
      };
      struct Reset {
        ~Reset() { storage::internal::repository_test_hooks = {}; }
      } reset;
      failed = run();
      return empty_result();
    };
  });
  ASSERT_TRUE(finished(update));
  auto query = submit(coordinator, ipc::BrowseRequest{core::RecordType::Memo, {}}, queried);
  EXPECT_TRUE(finished(query));
  coordinator.drain();
  EXPECT_EQ(catalog_coordinator.read_state().status, storage::CatalogStatus::Inconsistent);
  auto failure =
      std::get<app::MutationFailure>(std::get<ipc::NormalMutationResponse>(failed).result);
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::Applied);
  EXPECT_EQ(std::get<app::ApplicationResponse<app::BrowseResult>>(queried).warnings.size(), 1u);
  EXPECT_EQ(std::get<storage::MemoMetadata>(*old.find_record(memo.id)).metadata.title, "original");
}

TEST_F(ConcurrentOperations, IpcControlBrowseAndUnrelatedLoadProgressWhileAcceptedMutationRuns) {
  auto ownership_result = ipc::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<ipc::RepositoryOwnership>(ownership_result));
  auto ownership = std::get<ipc::RepositoryOwnership>(std::move(ownership_result));
  Gate hold;
  std::promise<void> entered;
  std::stop_source shutdown;
  std::exception_ptr error;
  ipc::FatalErrorReporter fatal_errors;
  ipc::RuntimeLimits limits{std::get<folio::config::EffectiveConfig>(
      folio::config::configuration_defaults(paths.application))};
  ipc::RequestCoordinator request_coordinator(
      limits.requests, limits.access_workers,
      [&](std::exception_ptr error) { fatal_errors.report(error); },
      [&](ipc::OperationRequest request,
          ipc::Session session) -> ipc::RequestCoordinator::OperationAction {
        const bool mutation = std::holds_alternative<ipc::MutationRequest>(request);
        auto run = mutation || std::holds_alternative<ipc::AcquireEditRequest>(request) ||
                           std::holds_alternative<ipc::ReleaseEditRequest>(request)
                       ? mutations.prepare(std::move(request), std::move(session))
                       : access.prepare(std::move(request));
        if (!mutation) return run;
        return [run = std::move(run), wait = hold.ready, &entered] {
          entered.set_value();
          wait.wait();
          return run();
        };
      });
  ipc::IpcServer server(request_coordinator, fatal_errors, ownership, "", limits);
  std::thread thread([&] {
    try {
      EXPECT_TRUE(std::holds_alternative<std::monostate>(server.run(shutdown.get_token())));
      server.stop_admission();
      request_coordinator.drain();
      server.wait_for_responses();
      server.close_connections();
    } catch (...) {
      error = std::current_exception();
    }
  });
  struct Cleanup {
    std::function<void()> run;
    ~Cleanup() { run(); }
  } cleanup{[&] {
    hold.open();
    shutdown.request_stop();
    thread.join();
    EXPECT_FALSE(error);
  }};
  const auto settings = std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths));
  auto connect = [&] {
    return std::get<app::BackendConnectionHandle>(app::BackendConnectionHandle::connect(settings));
  };
  auto editing = connect(), browsing = connect(), loading = connect(), acquiring = connect();
  ASSERT_TRUE(std::holds_alternative<std::monostate>(editing.acquire_edit(memo.id)));
  auto save = std::async(std::launch::async, [&] {
    return editing.update(app::MutationId::generate(), memo.id,
                          app::MemoEditableFields{"changed", "body", {}}, time);
  });
  // Release before the async future's destructor if an assertion exits early.
  Cleanup release_save{[&] { hold.open(); }};
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  auto browse = browsing.browse(core::RecordType::Memo, {});
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(browse));
  EXPECT_EQ(std::get<core::MemoMetadata>(
                std::get<app::ApplicationResponse<app::BrowseResult>>(browse).value.records.at(0))
                .title,
            "original");
  auto denied = acquiring.acquire_edit(memo.id);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(denied));
  EXPECT_EQ(std::get<app::ApplicationError>(denied).reason, app::ErrorReason::LockConflict);
  auto missing = loading.load_memo(core::Id::generate());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(missing));
  EXPECT_EQ(std::get<app::ApplicationError>(missing).reason, app::ErrorReason::NotFound);
  EXPECT_EQ(save.wait_for(0ms), std::future_status::timeout);
  hold.open();
  auto saved = save.get();
  ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::MemoMetadata>>(saved));
  EXPECT_TRUE(std::holds_alternative<core::MemoMetadata>(
      std::get<app::MutationResponse<core::MemoMetadata>>(saved).result));
  auto loaded = loading.load_memo(memo.id);
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  EXPECT_EQ(std::get<core::Memo::Snapshot>(loaded).title, "changed");
  EXPECT_TRUE(fatal_errors.take_failures().empty());
}

TEST_F(ConcurrentOperations, RepeatedReadStartsObserveAtomicMetadataAndMembershipPublication) {
  auto created = std::get<ipc::NormalMutationResponse>(
      mutate(ipc::CreateDirectoryRequest{core::DirectoryKind::Memo, "destination"}));
  auto directory = std::get<core::DirectoryMetadata>(std::get<ipc::MutationValue>(created.result));
  std::promise<void> started;
  auto writer = std::async(std::launch::async, [&] {
    started.set_value();
    for (unsigned i = 0; i < 64; ++i) {
      auto result = std::get<ipc::NormalMutationResponse>(mutate(
          ipc::MoveMemoRequest{memo.id, i % 2 ? std::optional<core::Id>{} : directory.id, time}));
#ifndef _WIN32
      EXPECT_TRUE(std::holds_alternative<ipc::MutationValue>(result.result));
#endif
    }
  });
  started.get_future().wait();
  for (unsigned i = 0; i < 1000; ++i) {
    auto state = catalog_coordinator.read_state();
    EXPECT_EQ(state.status, storage::CatalogStatus::Ready);
    const auto metadata =
        std::get<storage::MemoMetadata>(*state.view.find_record(memo.id)).metadata;
    const auto root = state.view.list_root(core::RecordType::Memo);
    const auto members = std::get<std::vector<storage::RecordMetadata>>(
        state.view.list_directory(core::DirectoryKind::Memo, directory.id));
    EXPECT_EQ(root.records.size(), metadata.directory_id ? 0u : 1u);
    EXPECT_EQ(members.size(), metadata.directory_id ? 1u : 0u);
  }
  writer.get();
}

TEST_F(ConcurrentOperations, AcceptedQueuedMutationRetainsDisconnectedOwnerUntilExecutionSettles) {
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::AcquireEditRequest{memo.id}, owner)));
  ipc::RequestCoordinator coordinator(8);
  Gate hold;
  std::promise<void> busy;
  auto blocker = coordinator.admit({Lane::Mutation, "unrelated"}, [&] {
    return [&, wait = hold.ready] {
      busy.set_value();
      wait.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(busy.get_future().wait_for(2s), std::future_status::ready);
  ipc::OperationResponse saved, control;
  auto update = coordinator.admit({Lane::Mutation, memo.id.to_string()}, [&, session = owner] {
    auto run = mutations.prepare(
        ipc::MutationRequest{
            app::MutationId::generate(),
            ipc::UpdateMemoRequest{memo.id, {"saved after disconnect", "body", {}}, time}},
        session);
    return [&, run] {
      saved = run();
      return empty_result();
    };
  });
  owner.reset(); // Accepted preparation is now the only owner of this connection session.
  auto denied = submit(coordinator, ipc::AcquireEditRequest{memo.id}, control, other);
  ASSERT_TRUE(finished(denied));
  EXPECT_EQ(std::get<app::ApplicationError>(control).reason, app::ErrorReason::LockConflict);
  hold.open();
  ASSERT_TRUE(finished(update));
  EXPECT_TRUE(std::holds_alternative<ipc::MutationValue>(
      std::get<ipc::NormalMutationResponse>(saved).result));
  auto acquire = submit(coordinator, ipc::AcquireEditRequest{memo.id}, control, other);
  EXPECT_TRUE(finished(acquire));
  coordinator.drain();
  EXPECT_TRUE(std::holds_alternative<std::monostate>(control));
  EXPECT_TRUE(finished(blocker));
}

} // namespace

namespace {
TEST(ConcurrentMaintenance, RefreshDefersMutationsAllowsAccessAndSharesRunningResult) {
  ipc::RequestCoordinator coordinator(8, 2);
  Gate mutation_gate, refresh_gate;
  std::promise<void> mutation_started, refresh_started;
  std::atomic<int> scans = 0, writes = 0;
  auto mutation = coordinator.admit({Lane::Mutation, "A"}, [&] {
    return [&] {
      mutation_started.set_value();
      mutation_gate.ready.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(mutation_started.get_future().wait_for(2s), std::future_status::ready);
  auto refresh = coordinator.admit({Lane::Maintenance, {}, true, {}, true}, [&] {
    return [&] {
      ++scans;
      refresh_started.set_value();
      refresh_gate.ready.wait();
      return empty_result();
    };
  });
  auto deferred = coordinator.admit({Lane::Mutation, "B"}, [&] {
    return [&] {
      ++writes;
      return empty_result();
    };
  });
  auto read = coordinator.admit({Lane::Query, {}}, [] { return [] { return empty_result(); }; });
  EXPECT_TRUE(finished(read));
  EXPECT_EQ(scans.load(), 0);
  EXPECT_EQ(writes.load(), 0);
  mutation_gate.open();
  ASSERT_EQ(refresh_started.get_future().wait_for(2s), std::future_status::ready);
  auto joined = coordinator.admit({Lane::Maintenance, {}, true, {}, true}, [&] {
    return [&] {
      ++scans;
      return empty_result();
    };
  });
  ASSERT_TRUE(joined);
  auto another_read =
      coordinator.admit({Lane::Load, "A"}, [] { return [] { return empty_result(); }; });
  EXPECT_TRUE(finished(another_read));
  EXPECT_EQ(writes.load(), 0);
  refresh_gate.open();
  coordinator.drain();
  EXPECT_TRUE(finished(refresh));
  EXPECT_TRUE(finished(joined));
  EXPECT_TRUE(finished(deferred));
  EXPECT_EQ(scans.load(), 1);
  EXPECT_EQ(writes.load(), 1);
}
TEST(ConcurrentMaintenance, CheckReservationEndsOnFailureAndDoesNotBlockAccess) {
  ipc::RequestCoordinator coordinator(8, 2);
  Gate gate;
  std::promise<void> checking;
  std::atomic<bool> mutated = false;
  auto check = coordinator.admit({Lane::Maintenance, {}}, [&] {
    return [&]() -> ipc::RequestCoordinator::Result {
      checking.set_value();
      gate.ready.wait();
      throw std::runtime_error("check failed");
    };
  });
  ASSERT_EQ(checking.get_future().wait_for(2s), std::future_status::ready);
  auto mutation = coordinator.admit({Lane::Mutation, {}}, [&] {
    return [&] {
      mutated = true;
      return empty_result();
    };
  });
  auto query = coordinator.admit({Lane::Query, {}}, [] { return [] { return empty_result(); }; });
  EXPECT_TRUE(finished(query));
  EXPECT_FALSE(mutated.load());
  gate.open();
  coordinator.drain();
  EXPECT_TRUE(finished(check));
  EXPECT_TRUE(finished(mutation));
  EXPECT_TRUE(mutated.load());
  std::lock_guard lock(check->mutex);
  EXPECT_TRUE(check->exception);
}
TEST(ConcurrentMaintenance, CoalescedRequestsCountAgainstAdmissionAndFailureReachesEveryTicket) {
  ipc::RequestCoordinator coordinator(2, 1);
  Gate gate;
  std::promise<void> started;
  auto first = coordinator.admit({Lane::Maintenance, {}, true, {}, true}, [&] {
    return [&]() -> ipc::RequestCoordinator::Result {
      started.set_value();
      gate.ready.wait();
      throw std::runtime_error("refresh failed");
    };
  });
  ASSERT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
  auto second = coordinator.admit({Lane::Maintenance, {}, true, {}, true},
                                  [] { return [] { return empty_result(); }; });
  ASSERT_TRUE(second);
  EXPECT_FALSE(coordinator.admit({Lane::Maintenance, {}, true, {}, true},
                                 [] { return [] { return empty_result(); }; }));
  coordinator.stop_admission();
  gate.open();
  coordinator.drain();
  for (const auto& ticket : {first, second}) {
    EXPECT_TRUE(finished(ticket));
    std::lock_guard lock(ticket->mutex);
    EXPECT_TRUE(ticket->exception);
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(*ticket->result));
  }
}
} // namespace

namespace {
TEST(ConcurrentMaintenance, CheckAndRefreshUseOrdinaryEligibleSelectionUntilPrioritySystemExists) {
  ipc::RequestCoordinator coordinator(8, 2);
  Gate writer_gate, check_gate;
  std::promise<void> writer_started, check_started;
  std::atomic<bool> refreshed = false;
  coordinator.admit({Lane::Mutation, {}}, [&] {
    return [&] {
      writer_started.set_value();
      writer_gate.ready.wait();
      return empty_result();
    };
  });
  ASSERT_EQ(writer_started.get_future().wait_for(2s), std::future_status::ready);
  auto check = coordinator.admit({Lane::Maintenance, {}}, [&] {
    return [&] {
      check_started.set_value();
      check_gate.ready.wait();
      return empty_result();
    };
  });
  auto refresh = coordinator.admit({Lane::Maintenance, {}, true, {}, true}, [&] {
    return [&] {
      refreshed = true;
      return empty_result();
    };
  });
  writer_gate.open();
  ASSERT_EQ(check_started.get_future().wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(refreshed.load());
  check_gate.open();
  coordinator.drain();
  EXPECT_TRUE(finished(check));
  EXPECT_TRUE(finished(refresh));
  EXPECT_TRUE(refreshed.load());
}
} // namespace

namespace {
TEST(ConcurrentMaintenance, ChecksSharePendingExecutionButDoNotCachePolicyInCoordinator) {
  for (bool fail : {false, true}) {
    ipc::RequestCoordinator coordinator(8, 2);
    Gate gate;
    std::promise<void> started;
    std::atomic<int> attempts = 0;
    auto plan = ipc::RequestCoordinator::plan(ipc::CheckConsistencyRequest{core::RecordType::Memo});
    auto prepare = [&] {
      return [&]() -> ipc::RequestCoordinator::Result {
        ++attempts;
        started.set_value();
        gate.ready.wait();
        if (fail) throw std::runtime_error("check failed");
        return empty_result();
      };
    };
    auto first = coordinator.admit(plan, prepare);
    ASSERT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
    auto second = coordinator.admit(plan, prepare);
    ASSERT_TRUE(second);
    gate.open();
    EXPECT_TRUE(finished(first));
    EXPECT_TRUE(finished(second));
    auto repeated = coordinator.admit(plan, [&] {
      return [&] {
        ++attempts;
        return empty_result();
      };
    });
    ASSERT_TRUE(repeated);
    EXPECT_TRUE(finished(repeated));
    EXPECT_EQ(attempts.load(), 2);
    auto other = coordinator.admit(
        ipc::RequestCoordinator::plan(ipc::CheckConsistencyRequest{core::RecordType::Todo}), [&] {
          return [&] {
            ++attempts;
            return empty_result();
          };
        });
    EXPECT_TRUE(finished(other));
    EXPECT_EQ(attempts.load(), 3);
    coordinator.drain();
  }
}
} // namespace

namespace {
TEST(ConcurrentMaintenance, SharedLanePreservesRefreshAndCheckSchedulingIdentity) {
  const auto check =
      ipc::RequestCoordinator::plan(ipc::CheckConsistencyRequest{core::RecordType::Memo});
  const auto full = ipc::RequestCoordinator::plan(ipc::RefreshRequest{});
  const auto scoped = ipc::RequestCoordinator::plan(
      ipc::RefreshRequest{folio::application::DirectoryScanScope{core::RecordType::Memo}});
  EXPECT_EQ(check.lane, Lane::Maintenance);
  EXPECT_EQ(full.lane, check.lane);
  EXPECT_EQ(scoped.lane, check.lane);
  EXPECT_FALSE(check.refresh);
  EXPECT_TRUE(check.check_scope);
  EXPECT_TRUE(full.refresh);
  EXPECT_TRUE(full.full_refresh);
  EXPECT_TRUE(scoped.refresh);
  EXPECT_FALSE(scoped.full_refresh);
}
} // namespace
