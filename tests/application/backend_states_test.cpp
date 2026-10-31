#include "application/internal/backend_states.hpp"
#include "application/internal/configuration_test_hooks.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <iostream>
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
    base = fs::temp_directory_path() /
        ("fbstate-" + core::Id::generate().to_string().substr(0, 8));
    fs::create_directory(base);
    base = fs::canonical(base);
    paths = {base / "normal", base / "vault",
             {base / "config", base / "data", base / "state",
              base / "cache", base / "runtime"}};
  }

  void TearDown() override {
    context.coordinator.reset();
    context.repository.reset();
    context.ownership.reset();
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }

  BackendActionOutcome bootstrap() {
    testing::ScopedConfigurationOverride configuration(paths);
    return Bootstrap(context);
  }

  fs::path marker() { return paths.application.state / "session.marker"; }

  void block_index_save() {
    fs::create_directories(paths.application.cache);
    std::ofstream(paths.application.cache / "index") << "blocks index directory";
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
  testing::ScopedConfigurationOverride configuration(invalid);
  EXPECT_EQ(Bootstrap(context), BackendActionOutcome::Failure);
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Bootstrap);
  EXPECT_TRUE(std::holds_alternative<ProcessError>(context.diagnostics[0].error));
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
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  expect_released();
}

TEST_F(BackendStates, NormalFinishPersistsAndReleasesResources) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_EQ(Stopping(context), BackendActionOutcome::Success);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_TRUE(context.diagnostics.empty());
  EXPECT_FALSE(fs::exists(marker()));
  EXPECT_TRUE(fs::exists(paths.application.cache / "index/normal.msgpack"));
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
  testing::ScopedConfigurationOverride configuration(paths);
  BackendStateMachine machine(transitions, std::move(actions),
      [](State, std::exception_ptr) {
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
      {storage::StorageErrorCode::IoError, paths.normal_root, "partial mutation", {}, true}};
  context.coordinator->memo_update_callback()(std::cref(uncertain));
  ASSERT_EQ(context.coordinator->status(), storage::CatalogStatus::Inconsistent);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Failure);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  ASSERT_EQ(context.diagnostics.size(), 1u);
  EXPECT_EQ(context.diagnostics[0].phase, Phase::Finish);
  expect_released();
}

TEST_F(BackendStates, FailedLocalCleanupOutputDoesNotPreventRelease) {
  ASSERT_EQ(bootstrap(), BackendActionOutcome::Success);
  block_index_save();
  class FailingBuffer : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    int_type overflow(int_type) override { return traits_type::eof(); }
  } buffer;
  const auto previous_exceptions = std::cerr.exceptions();
  const auto previous_state = std::cerr.rdstate();
  auto* previous_buffer = std::cerr.rdbuf(&buffer);
  std::cerr.exceptions(std::ios::badbit | std::ios::failbit);
  BackendActionOutcome result = BackendActionOutcome::Success;
  try {
    result = Finish(context);
  } catch (...) {
    std::cerr.exceptions(std::ios::goodbit);
    std::cerr.rdbuf(previous_buffer);
    std::cerr.clear(previous_state);
    std::cerr.exceptions(previous_exceptions);
    throw;
  }
  std::cerr.exceptions(std::ios::goodbit);
  std::cerr.rdbuf(previous_buffer);
  std::cerr.clear(previous_state);
  std::cerr.exceptions(previous_exceptions);
  EXPECT_EQ(result, BackendActionOutcome::Failure);
  ASSERT_EQ(context.diagnostics.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(context.diagnostics.back().error));
  EXPECT_TRUE(fs::exists(marker()));
  expect_released();
}
}  // namespace
}  // namespace folio::application::internal
