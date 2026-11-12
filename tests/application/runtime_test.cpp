#include "push_fixture.hpp"
#include "execution/request_coordinator.hpp"
#include "ipc/ipc_server.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include "backend_connection_handle.hpp"

#include <gtest/gtest.h>
#include <condition_variable>
#include <future>

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(RequestCoordinator, SharedSequenceCapacityAndDrainSurviveLostDelivery) {
  ipc::RequestCoordinator coordinator(2);
  std::promise<void> started, release;
  auto gate = release.get_future().share();
  std::mutex mutex;
  std::vector<int> executed;
  auto first = coordinator.admit([&]() -> ipc::RequestCoordinator::Result {
    started.set_value();
    gate.wait();
    std::lock_guard lock(mutex);
    executed.push_back(1);
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  ASSERT_TRUE(first);
  started.get_future().wait();
  auto second = coordinator.admit([&]() -> ipc::RequestCoordinator::Result {
    std::lock_guard lock(mutex);
    executed.push_back(2);
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  EXPECT_TRUE(second);
  EXPECT_FALSE(coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    return ipc::Message{ipc::MessageKind::Response, {}};
  }));
  first.reset(); // Loss of a delivery owner does not retract accepted work.
  coordinator.stop_admission();
  release.set_value();
  coordinator.drain();
  EXPECT_EQ(executed, (std::vector<int>{1, 2}));
  ASSERT_TRUE(second->completed.stop_requested());
  EXPECT_TRUE(second->result);
  EXPECT_FALSE(coordinator.admit({}));
}

TEST(RequestCoordinator, ExceptionalExecutionDoesNotDropFollowingAcceptedWork) {
  ipc::RequestCoordinator coordinator(2);
  auto failed = coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    throw std::runtime_error("synthetic operation exception");
  });
  auto following = coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  coordinator.drain();
  ASSERT_TRUE(failed->result);
  EXPECT_TRUE(failed->exception);
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(*failed->result));
  ASSERT_TRUE(following->result);
  EXPECT_TRUE(std::holds_alternative<ipc::Message>(*following->result));
}

TEST(RequestCoordinator, DrainCollectsMultipleFailuresAndStillCompletesFollowingWork) {
  ipc::RequestCoordinator coordinator(4);
  std::promise<void> started, release;
  auto gate = release.get_future().share();
  auto first = coordinator.admit([&]() -> ipc::RequestCoordinator::Result {
    started.set_value();
    gate.wait();
    throw std::runtime_error("first synthetic failure");
  });
  started.get_future().wait();
  auto second = coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    throw std::logic_error("second synthetic failure");
  });
  auto third = coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    return ipc::IpcError{ipc::IpcErrorCode::IoError, "synthetic returned error"};
  });
  auto last = coordinator.admit([]() -> ipc::RequestCoordinator::Result {
    return ipc::Message{ipc::MessageKind::Response, {}};
  });
  release.set_value();
  ASSERT_NO_THROW(coordinator.drain());
  EXPECT_NO_THROW(coordinator.drain());
  for (const auto& ticket : {first, second, third, last}) {
    ASSERT_TRUE(ticket);
    EXPECT_TRUE(ticket->completed.stop_requested());
    EXPECT_EQ(ticket->stage, ipc::RequestCoordinator::ExecutionStage::Finished);
    ASSERT_TRUE(ticket->result);
  }
  ASSERT_TRUE(first->exception);
  ASSERT_TRUE(second->exception);
  EXPECT_THROW(std::rethrow_exception(first->exception), std::runtime_error);
  EXPECT_THROW(std::rethrow_exception(second->exception), std::logic_error);
  EXPECT_FALSE(third->exception);
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(*third->result));
  EXPECT_TRUE(std::holds_alternative<ipc::Message>(*last->result));
}

class IpcServer : public testing::Test {
protected:
  fs::path base = fs::canonical(fs::temp_directory_path()) /
                  ("frun-" + core::Id::generate().to_string().substr(0, 8));
  folio::config::StoragePaths paths{base / "normal",
                                    base / "vault",
                                    {base / "config", base / "data", base / "state", base / "cache",
                                     base / "logs", base / "runtime"}};
  std::optional<ipc::RepositoryOwnership> ownership;
  ipc::FatalErrorReporter fatal_errors;
  std::unique_ptr<ipc::RequestCoordinator> request_coordinator;
  std::unique_ptr<ipc::IpcServer> runtime;
  std::jthread server;
  std::stop_source shutdown;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<core::Id> executed;
  bool released = false, stopped = false;
  bool hold_report = false, drained = false;
  bool fail_operations = false, expect_failures = false;
  bool mismatch_mutation = false;
  ipc::RuntimeLimits limits{std::get<folio::config::EffectiveConfig>(
      folio::config::configuration_defaults(paths.application))};
  std::optional<ipc::IpcError> server_error;
  std::exception_ptr server_exception;
  std::vector<ipc::FatalErrorReporter::Failure> additional_failures;

  void start() {
    auto acquired = ipc::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<ipc::RepositoryOwnership>(acquired));
    ownership.emplace(std::get<ipc::RepositoryOwnership>(std::move(acquired)));
    request_coordinator = std::make_unique<ipc::RequestCoordinator>(
        limits.requests, limits.access_workers,
        [&](std::exception_ptr error) { fatal_errors.report(error); },
        ipc::RequestCoordinator::PrepareOperation{},
        [&](const ipc::OperationRequest& request, const ipc::Session&) -> ipc::OperationResponse {
          std::unique_lock lock(mutex);
          if (const auto* load = std::get_if<ipc::LoadMemoRequest>(&request))
            executed.push_back(load->id);
          else
            executed.push_back(core::Id::generate());
          changed.notify_all();
          changed.wait(lock, [&] { return released; });
          if (fail_operations) throw std::runtime_error("synthetic operation exception");
          if (const auto* mutation = std::get_if<ipc::MutationRequest>(&request)) {
            return ipc::NormalMutationResponse{
                mismatch_mutation ? app::MutationId::generate() : mutation->mutation_id,
                app::MutationFailure{
                    {app::ErrorCategory::InvalidRequest,
                     app::ErrorReason::InvalidMutation,
                     "Synthetic mutation failure",
                     {app::ErrorAction::Dismiss}},
                    app::AuthoritativeChangeStatus::NotApplied,
                    {executed.back(),
                     {},
                     {},
                     {{app::MutationStep::WriteEntry, app::StepObservation::NotAttempted}}}}};
          }
          return app::ApplicationError{app::ErrorCategory::TargetUnavailable,
                                       app::ErrorReason::NotFound,
                                       "Synthetic missing Memo",
                                       {app::ErrorAction::Dismiss}};
        });
    runtime = std::make_unique<ipc::IpcServer>(*request_coordinator, fatal_errors, *ownership, "",
                                               limits);
    server = std::jthread([&] {
      try {
        auto result = runtime->run(shutdown.get_token());
        if (auto* error = std::get_if<ipc::IpcError>(&result)) server_error = *error;
      } catch (...) {
        server_exception = std::current_exception();
      }
      runtime->stop_admission();
      request_coordinator->drain();
      runtime->wait_for_responses();
      {
        std::unique_lock lock(mutex);
        drained = true;
        changed.notify_all();
        changed.wait(lock, [&] { return !hold_report; });
      }
      runtime->close_connections();
      additional_failures = fatal_errors.take_failures();
      std::lock_guard lock(mutex);
      stopped = true;
      changed.notify_all();
    });
  }

  ipc::ConnectionSettings settings() {
    return std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths));
  }
  std::vector<std::unique_ptr<folio::test::PushPeer>> push_receivers;
  ipc::IpcStream connect() {
    auto connected = ipc::IpcStream::connect(settings().endpoint);
    if (!std::holds_alternative<ipc::IpcStream>(connected))
      throw std::runtime_error("connect failed");
    auto stream = std::get<ipc::IpcStream>(std::move(connected));
    auto ready = ipc::exchange_message(
        stream,
        {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                      ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
        Clock::now() + 3s);
    if (!std::holds_alternative<ipc::Message>(ready) ||
        std::get<ipc::Message>(ready).kind != ipc::MessageKind::Ready)
      throw std::runtime_error("handshake failed");
    push_receivers.push_back(folio::test::pair_push(settings(), std::get<ipc::Message>(ready)));
    return stream;
  }
  ipc::Message receive(ipc::IpcStream& stream) {
    ipc::MessageTransport transport;
    auto message = ipc::receive_message(stream, transport, Clock::now() + 3s);
    if (!std::holds_alternative<ipc::Message>(message)) throw std::runtime_error("receive failed");
    return std::get<ipc::Message>(std::move(message));
  }
  void submit(ipc::IpcStream& stream, const core::Id& id) {
    const auto bytes =
        std::get<std::string>(ipc::codec::encode<ipc::OperationRequest>(ipc::LoadMemoRequest{id}));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(ipc::send_message(
        stream, {ipc::MessageKind::Request, ipc::payload_bytes(bytes)}, Clock::now() + 3s)));
  }
  bool await_count(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 3s, [&] { return executed.size() >= count; });
  }
  bool await_stop() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 4s, [&] { return stopped; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
  void TearDown() override {
    {
      std::lock_guard lock(mutex);
      hold_report = false;
      changed.notify_all();
    }
    release();
    shutdown.request_stop();
    if (server.joinable()) server.join();
    runtime.reset();
    ownership.reset();
    EXPECT_FALSE(server_error) << (server_error ? server_error->message : "");
    if (!expect_failures) {
      EXPECT_FALSE(server_exception);
      EXPECT_TRUE(additional_failures.empty());
    }
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
};

TEST_F(IpcServer, AcceptanceIsVisibleAndOtherConnectionsRemainResponsiveDuringExecution) {
  start();
  auto first = connect(), second = connect(), health = connect();
  auto a = core::Id::generate(), b = core::Id::generate();
  submit(first, a);
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(second, b);
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Accepted);
  // Executor is deliberately blocked, but another worker handles health IPC.
  auto pong = ipc::exchange_message(health, {ipc::MessageKind::Ping, {}}, Clock::now() + 2s);
  ASSERT_TRUE(std::holds_alternative<ipc::Message>(pong));
  EXPECT_EQ(std::get<ipc::Message>(pong).kind, ipc::MessageKind::Pong);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(executed, std::vector<core::Id>{a});
  }
  release();
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Response);
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Response);
  ASSERT_TRUE(await_count(2));
  EXPECT_EQ(executed, (std::vector<core::Id>{a, b}));
}

TEST_F(IpcServer, QueueRejectionIsDistinctAndLeavesConnectionUsable) {
  limits.requests = 1;
  start();
  auto first = connect(), second = connect();
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(second, core::Id::generate());
  auto rejected = receive(second);
  EXPECT_EQ(rejected.kind, ipc::MessageKind::Rejected);
  auto error = ipc::codec::decode<app::ApplicationError>(ipc::payload_text(rejected.payload));
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(error));
  EXPECT_EQ(std::get<app::ApplicationError>(error).reason, app::ErrorReason::ResourceLimit);
  auto pong = ipc::exchange_message(second, {ipc::MessageKind::Ping, {}}, Clock::now() + 2s);
  EXPECT_TRUE(std::holds_alternative<ipc::Message>(pong));
  release();
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Response);
}

TEST_F(IpcServer, HeaderReservationBoundsIncompleteRequestBuffers) {
  limits.buffered_requests = 1;
  start();
  auto first = connect(), second = connect();
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  const auto bytes =
      ipc::encode_message({ipc::MessageKind::Request, std::vector<std::byte>(128, std::byte{1})});
  auto sent = second.write(std::as_bytes(std::span(bytes.data(), 8)));
  ASSERT_TRUE(std::holds_alternative<std::size_t>(sent));
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Rejected);
  release();
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Response);
  EXPECT_EQ(executed.size(), 1u);
}

TEST_F(IpcServer, DisconnectAfterAcceptanceDoesNotCancelQueuedOrExecutingWork) {
  start();
  auto first = connect(), second = connect();
  const auto a = core::Id::generate(), b = core::Id::generate();
  submit(first, a);
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(second, b);
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Accepted);
  first = ipc::IpcStream{};
  second = ipc::IpcStream{};
  release();
  ASSERT_TRUE(await_stop());
  EXPECT_EQ(executed, (std::vector<core::Id>{a, b}));
  // Repo locks survive the drain until the lifecycle owner releases ownership.
  auto competing = ipc::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<app::ProcessError>(competing));
  EXPECT_EQ(std::get<app::ProcessError>(competing).code, app::ProcessErrorCode::AlreadyOwned);
}

TEST_F(IpcServer, ExecutionExceptionsRemainRetainedAfterTheirConnectionsAreLost) {
  fail_operations = expect_failures = true;
  start();
  auto first = connect(), second = connect();
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(second, core::Id::generate());
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Accepted);
  first = ipc::IpcStream{};
  second = ipc::IpcStream{};
  release();
  ASSERT_TRUE(await_stop());
  EXPECT_EQ(executed.size(), 2u);
  // The first exception may reach run() before the disconnect stops admission;
  // any remaining failures are collected after draining, never overwritten.
  EXPECT_EQ((server_exception ? 1u : 0u) + additional_failures.size(), 2u);
  for (const auto& failure : additional_failures)
    EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(failure));
}

TEST_F(IpcServer, ShutdownDrainsAcceptedWorkAndAttemptsFinalDelivery) {
  start();
  auto first = connect(), second = connect();
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(second, core::Id::generate());
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Accepted);
  shutdown.request_stop();
  release();
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Response);
  EXPECT_EQ(receive(second).kind, ipc::MessageKind::Response);
  ASSERT_TRUE(await_stop());
  EXPECT_EQ(executed.size(), 2u);
}

TEST_F(IpcServer, PipeliningWhileAcceptedIsRejectedWithoutCancellingOriginal) {
  start();
  auto first = connect(), keeper = connect();
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  submit(first, core::Id::generate());
  EXPECT_EQ(receive(first).kind, ipc::MessageKind::Reject);
  release();
  auto pong = ipc::exchange_message(keeper, {ipc::MessageKind::Ping, {}}, Clock::now() + 2s);
  EXPECT_TRUE(std::holds_alternative<ipc::Message>(pong));
  EXPECT_EQ(executed.size(), 1u);
}

TEST_F(IpcServer, BlockingWaitHasDeadlineAndPromptShutdownWakeup) {
  start();
  auto stream = connect();
  auto timeout = stream.wait(false, Clock::now() + 20ms);
  ASSERT_TRUE(std::holds_alternative<ipc::IpcError>(timeout));
  EXPECT_EQ(std::get<ipc::IpcError>(timeout).code, ipc::IpcErrorCode::Timeout);
  std::promise<void> entered;
  std::stop_source wake;
  auto waiting = std::async(std::launch::async, [&] {
    entered.set_value();
    return stream.wait(false, Clock::time_point::max(), wake.get_token());
  });
  entered.get_future().wait();
  EXPECT_EQ(waiting.wait_for(20ms), std::future_status::timeout);
  wake.request_stop();
  ASSERT_EQ(waiting.wait_for(1s), std::future_status::ready);
  auto interrupted = waiting.get();
  ASSERT_TRUE(std::holds_alternative<bool>(interrupted));
  EXPECT_FALSE(std::get<bool>(interrupted));
  shutdown.request_stop();
  ASSERT_TRUE(await_stop());
}

TEST_F(IpcServer, ConnectionCapacityWaitWakesWhenAWorkerLeaves) {
  limits.connections = 2;
  start();
  auto first = connect(), keeper = connect();
  auto waiting = std::async(std::launch::async,
                            [&] { return app::BackendConnectionHandle::connect(settings()); });
  EXPECT_EQ(waiting.wait_for(20ms), std::future_status::timeout);
  first = ipc::IpcStream{};
  ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready);
  auto connected = waiting.get();
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      std::get<app::BackendConnectionHandle>(connected).ping()));
}

TEST_F(IpcServer, BlockingPublicReadWaitsForFinalResultAfterAdmission) {
  start();
  auto result = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(result));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(result));
  auto read =
      std::async(std::launch::async, [&] { return connection.load_memo(core::Id::generate()); });
  ASSERT_TRUE(await_count(1));
  EXPECT_EQ(read.wait_for(20ms), std::future_status::timeout);
  release();
  auto response = read.get();
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(response));
  EXPECT_EQ(std::get<app::ApplicationError>(response).reason, app::ErrorReason::NotFound);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}
} // namespace

TEST_F(IpcServer, FailureDrainPreservesOtherConnectionsUntilFinish) {
  hold_report = true;
  fail_operations = true;
  expect_failures = true;
  start();
  auto failing = connect(), idle = connect();
  submit(failing, core::Id::generate());
  EXPECT_EQ(receive(failing).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  release();
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 3s, [&] { return drained; }));
  }
  EXPECT_TRUE(server_exception);
  // No terminal notification exists yet. Draining must not send EOF either.
  auto waiting = idle.wait(false, Clock::now() + 30ms);
  ASSERT_TRUE(std::holds_alternative<ipc::IpcError>(waiting));
  EXPECT_EQ(std::get<ipc::IpcError>(waiting).code, ipc::IpcErrorCode::Timeout);
  {
    std::lock_guard lock(mutex);
    hold_report = false;
    changed.notify_all();
  }
  ASSERT_TRUE(await_stop());
  EXPECT_FALSE(idle.connected());
}

TEST_F(IpcServer, MutationIdentityMismatchInvalidatesConnectionWithoutFabricatingFailure) {
  mismatch_mutation = true;
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  core::TimestampContext time{core::Timestamp{}, "UTC", {}};
  const auto id = app::MutationId::generate();
  auto request = std::async(std::launch::async, [&] {
    return connection.create(id, app::MemoEditableFields{"title", "body", {}}, time, time, {});
  });
  ASSERT_TRUE(await_count(1));
  release();
  auto result = request.get();
  ASSERT_TRUE(std::holds_alternative<app::NoMutationResponse>(result));
  const auto& missing = std::get<app::NoMutationResponse>(result);
  EXPECT_EQ(missing.mutation_id, id);
  EXPECT_EQ(missing.submission, app::SubmissionState::Accepted);
  EXPECT_EQ(missing.error.reason, app::ErrorReason::ProtocolError);
  EXPECT_FALSE(connection.connected());
}

TEST_F(IpcServer, MutationAdmissionRejectionRetainsIdentityAndIsNotFinalMutationFailure) {
  limits.requests = 1;
  start();
  auto blocker = connect();
  submit(blocker, core::Id::generate());
  EXPECT_EQ(receive(blocker).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  core::TimestampContext time{core::Timestamp{}, "UTC", {}};
  const auto id = app::MutationId::generate();
  auto result = connection.create(id, app::MemoEditableFields{"title", "body", {}}, time, time, {});
  ASSERT_TRUE(std::holds_alternative<app::NoMutationResponse>(result));
  EXPECT_EQ(std::get<app::NoMutationResponse>(result).submission, app::SubmissionState::Rejected);
  EXPECT_EQ(std::get<app::NoMutationResponse>(result).mutation_id, id);
  EXPECT_TRUE(connection.connected());
  release();
  EXPECT_EQ(receive(blocker).kind, ipc::MessageKind::Response);
}

TEST_F(IpcServer, LostMutationFinalResponseRetainsAcceptedSubmissionWithoutBackendStatus) {
  fail_operations = expect_failures = true;
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  core::TimestampContext time{core::Timestamp{}, "UTC", {}};
  const auto id = app::MutationId::generate();
  auto request = std::async(std::launch::async, [&] {
    return connection.create(id, app::MemoEditableFields{"title", "body", {}}, time, time, {});
  });
  ASSERT_TRUE(await_count(1));
  release();
  auto result = request.get();
  ASSERT_TRUE(std::holds_alternative<app::NoMutationResponse>(result));
  EXPECT_EQ(std::get<app::NoMutationResponse>(result).mutation_id, id);
  EXPECT_EQ(std::get<app::NoMutationResponse>(result).submission, app::SubmissionState::Accepted);
  EXPECT_FALSE(connection.connected());
}

TEST_F(IpcServer, NotificationProducerFailureReachesLifecycleWithoutClosingSessions) {
  hold_report = true;
  expect_failures = true;
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  runtime->publish(app::AvailabilityChangeNotice{
      app::SharedAvailability::Fatal,
      app::ApplicationError{app::ErrorCategory::ApplicationUnavailable,
                            app::ErrorReason::BackendUnavailable,
                            std::string(9000, 'x'),
                            {}}});
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 3s, [&] { return drained; }));
  }
  EXPECT_TRUE(connection.connected());
  runtime->publish(app::AvailabilityChangeNotice{app::SharedAvailability::Fatal});
  ASSERT_TRUE(folio::test::wait_push_notifications(connection, Clock::now() + 1s));
  auto events = connection.take_push_notifications();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(std::get<app::AvailabilityChangeNotice>(events.front()).state,
            app::SharedAvailability::Fatal);
  {
    std::lock_guard lock(mutex);
    hold_report = false;
    changed.notify_all();
  }
}

TEST_F(IpcServer, CancellingNotificationWaitLeavesSessionUsable) {
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  std::stop_source stop;
  auto waiting = std::async(std::launch::async,
                            [&] { return connection.wait_push_notifications(stop.get_token()); });
  stop.request_stop();
  EXPECT_FALSE(waiting.get());
  EXPECT_TRUE(connection.connected());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}

TEST_F(IpcServer, PushIsAckedWhileRequestExecutesAndRetainedUntilResult) {
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(connected));
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  const auto id = core::Id::generate();
  auto request = std::async(std::launch::async, [&] { return connection.load_memo(id); });
  ASSERT_TRUE(await_count(1));
  runtime->publish(app::PushNotification{app::RecordChangeNotice{id, true, false}});
  EXPECT_TRUE(connection.connected());
  EXPECT_TRUE(connection.take_push_notifications().empty());
  EXPECT_EQ(request.wait_for(0ms), std::future_status::timeout);
  release();
  EXPECT_TRUE(std::holds_alternative<app::ApplicationError>(request.get()));
  ASSERT_TRUE(folio::test::wait_push_notifications(connection, Clock::now() + 1s));
  auto events = connection.take_push_notifications();
  ASSERT_EQ(events.size(), 1u);
  const auto& invalidation = std::get<app::RecordChangeNotice>(events[0]);
  EXPECT_EQ(invalidation.id, id);
  EXPECT_TRUE(invalidation.metadata);
  EXPECT_FALSE(invalidation.content);
}

TEST_F(IpcServer, PushOrderIsIndependentOfUiConsumptionAndRequestTraffic) {
  start();
  auto connected = app::BackendConnectionHandle::connect(settings());
  auto connection = std::get<app::BackendConnectionHandle>(std::move(connected));
  const auto first = core::Id::generate(), second = core::Id::generate();
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{first}});
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{second}});
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  ASSERT_TRUE(folio::test::wait_push_notifications(connection, Clock::now() + 1s));
  auto events = connection.take_push_notifications();
  const auto deadline = Clock::now() + 1s;
  while (events.size() < 2 && folio::test::wait_push_notifications(connection, deadline)) {
    auto more = connection.take_push_notifications();
    events.insert(events.end(), more.begin(), more.end());
  }
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(std::get<app::EditLockChangeNotice>(events[0]).id, first);
  EXPECT_EQ(std::get<app::EditLockChangeNotice>(events[1]).id, second);
  EXPECT_TRUE(runtime->wait_for_push_delivery());
}

TEST_F(IpcServer, MissingPushAckTerminatesSessionButPreservesAcceptedWork) {
  limits.push_timeout = 40ms;
  start();
  auto request = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto token = std::get<ipc::Message>(ipc::exchange_message(
      request,
      {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                    ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
      Clock::now() + 3s));
  auto push = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  ASSERT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(
                    push,
                    {ipc::MessageKind::Hello,
                     ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
                         ipc::PushHello{*core::Id::parse(ipc::payload_text(token.payload))}})))},
                    Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Ready);
  submit(request, core::Id::generate());
  EXPECT_EQ(receive(request).kind, ipc::MessageKind::Accepted);
  ASSERT_TRUE(await_count(1));
  runtime->publish(
      app::PushNotification{app::AvailabilityChangeNotice{app::SharedAvailability::Ready}});
  EXPECT_EQ(receive(push).kind, ipc::MessageKind::Push);
  const auto delivery_start = Clock::now();
  EXPECT_FALSE(runtime->wait_for_push_delivery());
  EXPECT_LT(Clock::now() - delivery_start, 1s);
  ipc::MessageTransport input;
  auto closed = ipc::receive_message(request, input, Clock::now() + 3s);
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(closed));
  EXPECT_FALSE(request.connected());
  release();
  EXPECT_TRUE(await_stop());
  EXPECT_EQ(executed.size(), 1u);
}

TEST_F(IpcServer, NotificationOverflowTerminatesSessionWithoutWaitingForAckTimeout) {
  limits.pending_push_notification_capacity = 1;
  start();
  auto request = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto token = std::get<ipc::Message>(ipc::exchange_message(
      request,
      {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                    ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
      Clock::now() + 3s));
  auto push = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  ASSERT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(
                    push,
                    {ipc::MessageKind::Hello,
                     ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
                         ipc::PushHello{*core::Id::parse(ipc::payload_text(token.payload))}})))},
                    Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Ready);
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{core::Id::generate()}});
  EXPECT_EQ(receive(push).kind, ipc::MessageKind::Push);
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{core::Id::generate()}});
  ipc::MessageTransport input;
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::receive_message(request, input, Clock::now() + 1s)));
}

TEST_F(IpcServer, PairingTokenIsSingleUseAndCannotAttachAnUnrelatedSession) {
  start();
  auto request = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto token = std::get<ipc::Message>(ipc::exchange_message(
      request,
      {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                    ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
      Clock::now() + 3s));
  auto receiver = folio::test::pair_push(settings(), token);
  auto intruder = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto rejected = ipc::exchange_message(
      intruder,
      {ipc::MessageKind::Hello,
       ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
           ipc::PushHello{*core::Id::parse(ipc::payload_text(token.payload))}})))},
      Clock::now() + 3s);
  ASSERT_TRUE(std::holds_alternative<ipc::Message>(rejected));
  EXPECT_EQ(std::get<ipc::Message>(rejected).kind, ipc::MessageKind::Reject);
  EXPECT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(request, {ipc::MessageKind::Ping, {}}, Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Pong);
}

TEST_F(IpcServer, ShutdownWakesPushAckWaitAndDoesNotWaitForItsDeadline) {
  start();
  auto request = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto token = std::get<ipc::Message>(ipc::exchange_message(
      request,
      {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                    ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
      Clock::now() + 3s));
  auto push = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  ASSERT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(
                    push,
                    {ipc::MessageKind::Hello,
                     ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
                         ipc::PushHello{*core::Id::parse(ipc::payload_text(token.payload))}})))},
                    Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Ready);
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{core::Id::generate()}});
  EXPECT_EQ(receive(push).kind, ipc::MessageKind::Push);
  shutdown.request_stop();
  EXPECT_TRUE(await_stop());
}

TEST_F(IpcServer, IncompleteLargeRequestDoesNotOccupyPushIoWorker) {
  start();
  auto request = connect();
  const auto id = core::Id::generate();
  core::TimestampContext time{core::Timestamp{}, "UTC", {}};
  ipc::OperationRequest operation = ipc::MutationRequest{
      app::MutationId::generate(),
      ipc::CreateMemoRequest{
          app::MemoEditableFields{"title", std::string(256 * 1024, 'x'), {}}, time, time, {}}};
  const auto payload = std::get<std::string>(ipc::codec::encode(operation));
  const auto bytes = ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes(payload)});
  auto written = request.write(std::as_bytes(std::span(bytes.data(), 8)));
  ASSERT_TRUE(std::holds_alternative<std::size_t>(written));
  ASSERT_EQ(std::get<std::size_t>(written), 8u);
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{id}});
  ASSERT_TRUE(push_receivers.back()->wait(Clock::now() + 1s));
  EXPECT_EQ(push_receivers.back()->take().size(), 1u);
  {
    std::lock_guard lock(mutex);
    EXPECT_TRUE(executed.empty());
  }
  std::size_t offset = 8;
  while (offset < bytes.size()) {
    auto sent = request.write(std::as_bytes(std::span(bytes.data(), bytes.size())).subspan(offset));
    ASSERT_TRUE(std::holds_alternative<std::size_t>(sent));
    offset += std::get<std::size_t>(sent);
    if (offset < bytes.size())
      ASSERT_TRUE(std::holds_alternative<bool>(request.wait(true, Clock::now() + 3s)));
  }
  EXPECT_EQ(receive(request).kind, ipc::MessageKind::Accepted);
  release();
  EXPECT_EQ(receive(request).kind, ipc::MessageKind::Response);
}

TEST_F(IpcServer, WrongPushReceiptFailsOnlyItsOwnSession) {
  start();
  auto healthy = connect();
  auto request = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  auto token = std::get<ipc::Message>(ipc::exchange_message(
      request,
      {ipc::MessageKind::Hello, ipc::payload_bytes(std::get<std::string>(
                                    ipc::codec::encode(ipc::HelloPayload{ipc::RequestHello{}})))},
      Clock::now() + 3s));
  auto push = std::get<ipc::IpcStream>(ipc::IpcStream::connect(settings().endpoint));
  ASSERT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(
                    push,
                    {ipc::MessageKind::Hello,
                     ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
                         ipc::PushHello{*core::Id::parse(ipc::payload_text(token.payload))}})))},
                    Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Ready);
  runtime->publish(app::PushNotification{app::EditLockChangeNotice{core::Id::generate()}});
  auto envelope = receive(push);
  auto decoded = std::get<ipc::PushEnvelope>(
      ipc::codec::decode<ipc::PushEnvelope>(ipc::payload_text(envelope.payload)));
  const auto wrong =
      std::get<std::string>(ipc::codec::encode(ipc::PushReceipt{decoded.sequence + 1}));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(ipc::send_message(
      push, {ipc::MessageKind::PushAck, ipc::payload_bytes(wrong)}, Clock::now() + 3s)));
  ipc::MessageTransport input;
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::receive_message(request, input, Clock::now() + 1s)));
  EXPECT_EQ(std::get<ipc::Message>(
                ipc::exchange_message(healthy, {ipc::MessageKind::Ping, {}}, Clock::now() + 3s))
                .kind,
            ipc::MessageKind::Pong);
}
