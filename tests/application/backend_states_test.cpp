#include "runtime/backend_states.hpp"
#include <folio/ipc/application_codec.hpp>

#include <gtest/gtest.h>
#include <folio/logging/logging.hpp>
#include <spdlog/sinks/base_sink.h>
#include <fstream>
#include <future>
#include "blocking_frontend.hpp"
#include "frontend/runtime/connection_session.hpp"
#include "push_fixture.hpp"
#include <stdexcept>

namespace folio::application::internal {
namespace {
namespace fs = std::filesystem;
using Phase = BackendStateMachine::State;

class BackendStates : public ::testing::Test {
protected:
  fs::path base;
  storage::StoragePaths paths;
  BackendContext context;

  void SetUp() override {
    base = fs::temp_directory_path() / ("fbstate-" + core::Id::generate().to_string().substr(0, 8));
    fs::create_directory(base);
    base = fs::canonical(base);
    paths = {base / "normal",
             base / "vault",
             {base / "config", base / "data", base / "state", base / "cache", base / "logs",
              base / "runtime"}};
    ASSERT_TRUE(logging::initialize(logging::ProcessRole::Backend, paths.application.logs));
  }

  void TearDown() override {
    context.runtime.reset();
    context.coordinator.reset();
    context.repository.reset();
    context.ownership.reset();
    logging::shutdown();
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }

  BackendActionOutcome bootstrap() {
    context.startup_paths = paths;
    return Bootstrap(context);
  }

  fs::path marker() { return paths.application.state / "session.marker"; }

  void block_index_save() {
    fs::create_directories(paths.application.cache);
    std::ofstream(paths.application.state / "index") << "blocks index directory";
  }

  void expect_released() {
    EXPECT_FALSE(context.coordinator);
    EXPECT_FALSE(context.repository);
    EXPECT_FALSE(context.ownership);
    auto reacquired = RepositoryOwnership::acquire(paths);
    EXPECT_TRUE(std::holds_alternative<RepositoryOwnership>(reacquired));
  }
};

TEST_F(BackendStates, FailureBeforeOwnershipDoesNotStartSession) {
  auto invalid = paths;
  invalid.vault_root = invalid.normal_root;
  context.startup_paths = invalid;
  EXPECT_EQ(Bootstrap(context), BackendActionOutcome::Failure);
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Bootstrap);
  EXPECT_TRUE(
      std::holds_alternative<folio::config::ConfigurationError>(context.diagnostics[0].error));
  EXPECT_FALSE(context.ownership);
  EXPECT_FALSE(context.initialized);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_FALSE(fs::exists(marker()));
}

TEST_F(BackendStates, PartialStartupPreservesMarkerAndReleasesOwnership) {
  fs::create_directories(paths.application.state);
  std::ofstream(marker()) << "";
  fs::create_directories(paths.normal_root / "memos");
  std::ofstream(paths.normal_root / "memos/broken.md") << "invalid";
  EXPECT_EQ(bootstrap(), BackendActionOutcome::Failure);
  ASSERT_TRUE(context.ownership);
  EXPECT_FALSE(context.initialized);
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(context.diagnostics[0].error));
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_FALSE(fs::exists(paths.application.state / "index/normal.msgpack"));
  expect_released();
}

TEST_F(BackendStates, NormalFinishPersistsAndReleasesResources) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_EQ(Stopping(context), BackendActionOutcome::Success);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_TRUE(context.diagnostics.empty());
  EXPECT_FALSE(fs::exists(marker()));
  EXPECT_TRUE(fs::exists(paths.application.state / "index/normal.msgpack"));
  expect_released();
}

TEST_F(BackendStates, AcceptedWorkDrainsBeforeCatalogFinalizationAndOwnershipRelease) {
  using namespace std::chrono_literals;
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  std::promise<void> entered, release, stopping;
  auto gate = release.get_future().share();
  auto in_stopping = stopping.get_future();
  std::stop_source shutdown;
  std::jthread lifecycle, frontend;
  bool released = false;
  std::optional<folio::test::BlockingFrontend> handle;
  std::optional<AccessResult<Annotated<core::Memo::Snapshot>>> response;
  BackendActionOutcome stopped{}, finished{};
  struct Cleanup {
    std::function<void()> action;
    ~Cleanup() { action(); }
  } cleanup{[&] {
    shutdown.request_stop();
    if (!released) release.set_value();
    if (lifecycle.joinable()) lifecycle.join();
    if (frontend.joinable()) frontend.join();
  }};
  context.request_coordinator = std::make_unique<RequestCoordinator>(
      context.runtime_limits->requests, context.runtime_limits->access_workers,
      [&](std::exception_ptr error) { context.fatal_errors.report(error); },
      RequestCoordinator::PrepareOperation{},
      [&](const OperationRequest&, const Session&) -> OperationResponse {
        entered.set_value();
        gate.wait();
        return ApplicationError{ErrorCategory::TargetUnavailable,
                                ErrorReason::NotFound,
                                "Synthetic missing Memo",
                                {ErrorAction::Dismiss}};
      });
  context.runtime = std::make_unique<IpcServer>(*context.request_coordinator, context.fatal_errors,
                                                *context.ownership, "", *context.runtime_limits);
  lifecycle = std::jthread([&] {
    context.runtime->run(shutdown.get_token());
    stopping.set_value();
    stopped = Stopping(context);
    finished = Finish(context);
  });
  auto settings = std::get<ConnectionSettings>(connection_settings(paths));
  auto connected = folio::application::ConnectionSession::connect(settings);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<folio::application::ConnectionSession>>(connected));
  handle.emplace(std::get<std::unique_ptr<folio::application::ConnectionSession>>(connected));
  frontend = std::jthread([&] { response = handle->load_memo(core::Id::generate()); });
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  shutdown.request_stop();
  ASSERT_EQ(in_stopping.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(fs::exists(marker()));
  auto competing = RepositoryOwnership::acquire(paths);
  EXPECT_TRUE(std::holds_alternative<ProcessError>(competing));
  release.set_value();
  released = true;
  lifecycle.join();
  frontend.join();
  EXPECT_EQ(stopped, BackendActionOutcome::Success);
  EXPECT_EQ(finished, BackendActionOutcome::Success);
  EXPECT_FALSE(fs::exists(marker()));
  ASSERT_TRUE(response);
  ASSERT_TRUE(std::holds_alternative<ApplicationError>(*response));
  EXPECT_EQ(std::get<ApplicationError>(*response).reason, ErrorReason::NotFound);
  expect_released();
}

TEST_F(BackendStates, PrimaryAndFinalizationErrorsRetainTheirTypesAndOrder) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  const IpcError original{IpcErrorCode::ConnectionLost, "primary service failure",
                          std::make_error_code(std::errc::connection_reset)};
  record_backend_failure(context, {Phase::Serve, original});
  block_index_save();
  EXPECT_EQ(Finish(context), BackendActionOutcome::Failure);
  ASSERT_EQ(context.diagnostics.size(), 2u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Serve);
  const auto& primary = std::get<IpcError>(context.diagnostics[0].error);
  EXPECT_EQ(primary.message, original.message);
  EXPECT_EQ(primary.code, original.code);
  EXPECT_EQ(primary.system_error, original.system_error);
  EXPECT_EQ(context.diagnostics[1].phase, Phase::Finish);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(context.diagnostics[1].error));
  EXPECT_EQ(context.exit_code, 1);
  EXPECT_TRUE(fs::exists(marker()));
  expect_released();
}

TEST_F(BackendStates, FailedNotificationStillRetainsPrimaryBeforeCleanupFailure) {
  using State = BackendStateMachine::State;
  const BackendStateMachine::TransitionTable transitions{{
      {State::Bootstrap, State::Serve, State::Report},
      {State::Serve, State::Stopping, State::Report},
      {State::Stopping, State::Report, State::Report},
      {State::Report, State::Finish, State::Finish},
  }};
  auto actions = register_backend_states(context);
  actions[static_cast<std::size_t>(State::Serve)] = [&]() -> BackendActionOutcome {
    block_index_save();
    throw std::runtime_error("original serving exception");
  };
  context.startup_paths = paths;
  BackendStateMachine machine(transitions, std::move(actions), [](State, std::exception_ptr) {
    throw std::runtime_error("notification exception");
  });
  EXPECT_NO_THROW(machine.run());
  retain_backend_notification_failures(context, machine);
  ASSERT_EQ(context.diagnostics.size(), 3u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Serve);
  EXPECT_NE(diagnostic_message(context.diagnostics[0]).find("original serving exception"),
            std::string::npos);
  EXPECT_NE(diagnostic_message(context.diagnostics[1]).find("notification exception"),
            std::string::npos);
  EXPECT_EQ(context.diagnostics[2].phase, Phase::Finish);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(context.diagnostics[2].error));
  EXPECT_TRUE(fs::exists(marker()));
  expect_released();
}

TEST_F(BackendStates, InconsistentCatalogCannotBeFinalizedAsClean) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  storage::RepositoryMutationError uncertain{
      storage::MutationFailureStage::AuthoritativeStateMayHaveChanged,
      folio::io::FileError{
          folio::io::FileErrorCode::IoError, paths.normal_root, "partial mutation", {}, true}};
  context.coordinator->memo_update_callback()(std::cref(uncertain));
  ASSERT_EQ(context.coordinator->status(), storage::CatalogStatus::Inconsistent);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Failure);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_FALSE(fs::exists(paths.application.state / "index/normal.msgpack"));
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Finish);
  expect_released();
}

TEST_F(BackendStates, FailedLoggingDoesNotPreventReleaseOrChangeDiagnostics) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  block_index_save();
  class FailingSink : public spdlog::sinks::base_sink<std::mutex> {
    void sink_it_(const spdlog::details::log_msg&) override {
      throw spdlog::spdlog_ex("synthetic log failure");
    }
    void flush_() override {}
  };
  auto sink = std::make_shared<FailingSink>();
  auto logger = std::make_shared<spdlog::logger>("backend", sink);
  unsigned logging_failures = 0;
  logger->set_error_handler([&](const std::string&) { ++logging_failures; });
  auto previous = spdlog::default_logger();
  spdlog::set_default_logger(logger);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Failure);
  spdlog::set_default_logger(previous);
  EXPECT_EQ(logging_failures, 1u);
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<storage::CoordinatorError>(context.diagnostics.back().error));
  EXPECT_TRUE(fs::exists(marker()));
  expect_released();
}

TEST_F(BackendStates, LogsPrimaryAndCleanupFailuresWithoutSensitiveMessages) {
  const std::string sensitive = "synthetic-secret-body-never-log";
  record_backend_failure(
      context,
      {Phase::Bootstrap, config::ConfigurationError{
                             config::ConfigurationErrorCode::InvalidValue, {}, sensitive, {}}});
  record_backend_failure(context,
                         {Phase::Serve, std::make_exception_ptr(std::runtime_error(sensitive))});
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  block_index_save();
  EXPECT_EQ(Finish(context), BackendActionOutcome::Failure);
  spdlog::default_logger()->flush();
  std::string text;
  for (const auto& entry : fs::directory_iterator(paths.application.logs / "backend")) {
    std::ifstream in(entry.path());
    text.append(std::istreambuf_iterator<char>(in), {});
  }
  EXPECT_NE(text.find("primary failure during Bootstrap: ConfigurationError"), std::string::npos);
  EXPECT_NE(text.find("secondary failure during Finish: CoordinatorError"), std::string::npos);
  EXPECT_NE(text.find("[critical]"), std::string::npos);
  EXPECT_EQ(text.find(sensitive), std::string::npos);
  EXPECT_TRUE(fs::exists(marker()));
  expect_released();
}
} // namespace
} // namespace folio::application::internal

namespace folio::application::internal {
namespace {
TEST_F(BackendStates, FinishDrainsCoordinatorWhenIpcWasNotConstructed) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  context.request_coordinator = std::make_unique<RequestCoordinator>(
      context.runtime_limits->requests, context.runtime_limits->access_workers,
      [&](std::exception_ptr error) { context.fatal_errors.report(error); });
  bool executed = false;
  auto ticket = context.request_coordinator->admit([&]() -> RequestCoordinator::Result {
    executed = true;
    return Message{MessageKind::Response, {}};
  });
  ASSERT_TRUE(ticket);
  ASSERT_FALSE(context.runtime);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_TRUE(executed); // Draining joined execution before this observation.
  EXPECT_TRUE(ticket->completed.stop_requested());
}

TEST_F(BackendStates, ReportDeliversFatalAvailabilityBeforeFinishClosesSessions) {
  using namespace std::chrono_literals;
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  context.request_coordinator = std::make_unique<RequestCoordinator>(
      context.runtime_limits->requests, context.runtime_limits->access_workers,
      [&](std::exception_ptr error) { context.fatal_errors.report(error); });
  context.runtime = std::make_unique<IpcServer>(*context.request_coordinator, context.fatal_errors,
                                                *context.ownership, "", *context.runtime_limits);
  std::stop_source shutdown;
  std::jthread serving([&] { context.runtime->run(shutdown.get_token()); });
  auto settings = std::get<ConnectionSettings>(connection_settings(paths));
  auto connected = folio::application::ConnectionSession::connect(settings);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<folio::application::ConnectionSession>>(connected));
  auto handle_session = std::get<std::unique_ptr<folio::application::ConnectionSession>>(std::move(connected));
  folio::test::BlockingFrontend handle(handle_session);
  shutdown.request_stop();
  serving.join();
  record_backend_failure(
      context, {Phase::Serve,
                IpcError{IpcErrorCode::IoError, "private diagnostic must never appear in Push"}});
  ASSERT_EQ(Report(context), BackendActionOutcome::Success);
  ASSERT_TRUE(folio::test::wait_push_notifications(handle, std::chrono::steady_clock::now() + 1s));
  auto notifications = handle.connection_session().take();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_EQ(std::get<AvailabilityChangeNotice>(notifications[0]).state, SharedAvailability::Fatal);
  EXPECT_TRUE(std::get<AvailabilityChangeNotice>(notifications[0]).error);
  const auto bytes = std::get<std::string>(codec::encode(notifications[0]));
  EXPECT_EQ(bytes.find("private diagnostic"), std::string::npos);
  EXPECT_TRUE(handle.connected());
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
}
} // namespace
} // namespace folio::application::internal

namespace folio::application::internal {
TEST(FatalErrorReporter, RetainsCausesAndWakesWithoutIpcOwnership) {
  FatalErrorReporter reporter;
  auto wake = reporter.wake_token();
  std::stop_source observed;
  std::stop_callback callback(wake, [&] { observed.request_stop(); });
  std::jthread worker([&] {
    reporter.report(IpcError{IpcErrorCode::IoError, "synthetic fatal failure"});
    reporter.report(std::make_exception_ptr(std::runtime_error("synthetic exception")));
  });
  worker.join();
  EXPECT_TRUE(observed.stop_requested());
  auto first = reporter.take_one();
  ASSERT_TRUE(first);
  ASSERT_TRUE(std::holds_alternative<IpcError>(*first));
  EXPECT_EQ(std::get<IpcError>(*first).message, "synthetic fatal failure");
  auto remaining = reporter.take_failures();
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(remaining.front()));
  auto next = reporter.wake_token();
  EXPECT_FALSE(next.stop_requested());
  reporter.report(IpcError{IpcErrorCode::IoError, "next generation"});
  EXPECT_TRUE(next.stop_requested());
  // A failure submitted before registration must also be observable immediately.
  auto pending = reporter.wake_token();
  EXPECT_TRUE(pending.stop_requested());
}
} // namespace folio::application::internal
