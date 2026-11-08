#include <folio/logging/logging.hpp>
#include <folio/core/types.hpp>
#include "backend_connection_handle.hpp"
#include "logging/internal/session_file_test_access.hpp"
#include "runtime/backend_states.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <regex>

namespace {
namespace fs = std::filesystem;
namespace logging = folio::logging;
using Level = folio::config::LogLevel;

std::string read(const fs::path& file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

class Logging : public testing::Test {
protected:
  fs::path base;
  void SetUp() override {
    logging::shutdown();
#if defined(__APPLE__)
    const fs::path temporary = "/private/tmp"; // Keep Unix socket fixtures below sun_path limits.
#else
    const auto temporary = fs::temp_directory_path();
#endif
    base = temporary / ("flog-" + folio::core::Id::generate().to_string().substr(0, 8));
    fs::create_directory(base);
    base = fs::canonical(base);
#ifndef _WIN32
    fs::permissions(base, fs::perms::owner_all);
#endif
  }
  void TearDown() override {
    logging::shutdown();
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
};

TEST_F(Logging, BackendDefaultsToWarnAndErrorFlushesWithSourceLocation) {
  auto file = logging::initialize(logging::ProcessRole::Backend, base);
  ASSERT_TRUE(file);
  EXPECT_EQ(file->parent_path(), base / "backend");
  EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::warn);
  EXPECT_EQ(spdlog::default_logger()->flush_level(), spdlog::level::err);
  EXPECT_TRUE(std::regex_match(file->filename().string(),
                               std::regex(R"(\d{8}T\d{6}\.\d{3}Z-pid\d+\.log)")));
  SPDLOG_DEBUG("filtered-debug");
  SPDLOG_INFO("filtered-info");
  SPDLOG_WARN("retained-warning");
  SPDLOG_ERROR("retained-error");
  // Error must flush the preceding buffered warning without shutdown/flush here.
  const auto text = read(*file);
  EXPECT_EQ(text.find("filtered-"), std::string::npos);
  EXPECT_NE(text.find("retained-warning"), std::string::npos);
  EXPECT_NE(text.find("retained-error"), std::string::npos);
  EXPECT_NE(text.find("[backend]"), std::string::npos);
  EXPECT_TRUE(std::regex_search(
      text, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z \[error\])")));
  EXPECT_TRUE(std::regex_search(text, std::regex(R"(logging_test.cpp:\d+)")));
}

TEST_F(Logging, FrontendSessionAndLevelChangesKeepTheSameFile) {
  auto file = logging::initialize(logging::ProcessRole::Frontend, base);
  ASSERT_TRUE(file);
  auto* original = spdlog::default_logger().get();
  EXPECT_EQ(file->parent_path(), base / "frontend");
  const std::pair<Level, spdlog::level::level_enum> levels[] = {
      {Level::Trace, spdlog::level::trace}, {Level::Debug, spdlog::level::debug},
      {Level::Info, spdlog::level::info},   {Level::Warn, spdlog::level::warn},
      {Level::Error, spdlog::level::err},   {Level::Critical, spdlog::level::critical}};
  for (const auto& [setting, expected] : levels) {
    logging::set_level(setting);
    EXPECT_EQ(spdlog::default_logger()->level(), expected);
  }
  logging::set_level(Level::Trace);
  SPDLOG_TRACE("trace-enabled");
  SPDLOG_DEBUG("debug-enabled");
  SPDLOG_INFO("info-enabled");
  SPDLOG_CRITICAL("critical-enabled");
  EXPECT_EQ(spdlog::default_logger().get(), original);
  EXPECT_NE(read(*file).find("trace-enabled"), std::string::npos);
  EXPECT_NE(read(*file).find("debug-enabled"), std::string::npos);
  EXPECT_NE(read(*file).find("info-enabled"), std::string::npos);
  logging::set_level(Level::Error);
  SPDLOG_WARN("warning-filtered");
  SPDLOG_ERROR("error-enabled");
  EXPECT_EQ(read(*file).find("warning-filtered"), std::string::npos);
}

TEST_F(Logging, SessionReservationHasExactUtcNameAndNeverOverwrites) {
  using namespace std::chrono;
  const auto when = sys_days{year{2026} / October / 31} + 9h + 15min + 123ms;
  auto first = logging::internal::reserve_session_file(base, when, 1234);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->filename(), "20261031T091500.123Z-pid1234.log");
  std::ofstream(*first) << "preserve-existing-session";
  auto second = logging::internal::reserve_session_file(base, when, 1234);
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);
  EXPECT_EQ(second->filename(), "20261031T091500.123Z-pid1234-1.log");
  EXPECT_EQ(read(*first), "preserve-existing-session");
}

TEST_F(Logging, LaterSessionsPreserveHistoryAndShutdownFlushesWarnings) {
  auto first = logging::initialize(logging::ProcessRole::Frontend, base);
  ASSERT_TRUE(first);
  SPDLOG_WARN("first-session");
  logging::shutdown();
  const auto original = read(*first);
  EXPECT_NE(original.find("first-session"), std::string::npos);
  auto second = logging::initialize(logging::ProcessRole::Frontend, base);
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);
  SPDLOG_ERROR("second-session");
  EXPECT_EQ(read(*first), original);
  EXPECT_NE(read(*second).find("second-session"), std::string::npos);
}

TEST_F(Logging, MultilineDetailsKeepACompleteFirstLine) {
  auto file = logging::initialize(logging::ProcessRole::Backend, base);
  ASSERT_TRUE(file);
  SPDLOG_ERROR("Synthetic diagnostic title\n    Code: SyntheticFailure\n    Reason: test only");
  const auto text = read(*file);
  EXPECT_NE(
      text.find("Synthetic diagnostic title\n    Code: SyntheticFailure\n    Reason: test only"),
      std::string::npos);
}

TEST_F(Logging, FrontendLaunchFailureIsLoggedWithoutPayloadOrExceptionText) {
  auto file = logging::initialize(logging::ProcessRole::Frontend, base);
  ASSERT_TRUE(file);
#ifndef _WIN32
  const auto endpoint = base / "absent.sock";
#else
  const fs::path endpoint = L"\\\\.\\pipe\\folio-logging-test-" + base.filename().wstring();
#endif
  const auto failed = folio::application::connect_or_start_backend(
      folio::application::internal::ConnectionSettings{endpoint}, [] {
        return folio::core::Result<folio::application::LaunchedProcess,
                                   folio::application::ProcessError>{
            folio::application::ProcessError{folio::application::ProcessErrorCode::LaunchFailed,
                                             "synthetic-secret-launch-details",
                                             {}}};
      });
  EXPECT_TRUE(std::holds_alternative<folio::application::internal::IpcError>(failed));
  const auto text = read(*file);
  EXPECT_NE(text.find("Backend launch failed: ProcessError"), std::string::npos);
  EXPECT_EQ(text.find("synthetic-secret-launch-details"), std::string::npos);
}

TEST_F(Logging, RuntimeSinkFailureIsSilentAndDoesNotEscape) {
  ASSERT_TRUE(logging::initialize(logging::ProcessRole::Frontend, base));
  class FailingSink : public spdlog::sinks::base_sink<std::mutex> {
    void sink_it_(const spdlog::details::log_msg&) override {
      throw spdlog::spdlog_ex("synthetic write failure");
    }
    void flush_() override { throw spdlog::spdlog_ex("synthetic flush failure"); }
  };
  // Retain the production logger/error handler; replace only its failing device.
  spdlog::default_logger()->sinks() = {std::make_shared<FailingSink>()};
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(SPDLOG_ERROR("synthetic diagnostic"));
  EXPECT_NO_THROW(logging::shutdown());
  const auto stderr_output = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(stderr_output.empty());
}

TEST_F(Logging, ConfigurationFailureLeavesInitialWarnLoggerAvailable) {
  auto file = logging::initialize(logging::ProcessRole::Frontend, base / "logs");
  ASSERT_TRUE(file);
  auto* original = spdlog::default_logger().get();
  const folio::config::ApplicationPaths paths{base / "config", base / "data", base / "state",
                                              base / "cache",  base / "logs", std::nullopt};
  fs::create_directory(paths.config);
  const auto config_file = paths.config / "folio_config.toml";
  std::ofstream(config_file) << "[diagnostics]\nlog_level = 'invalid'\n";
  auto loaded = folio::config::load_configuration(paths, config_file);
  ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(loaded));
  EXPECT_EQ(spdlog::default_logger().get(), original);
  EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::warn);
  SPDLOG_ERROR("Synthetic configuration failure remains loggable");
  EXPECT_NE(read(*file).find("Synthetic configuration failure remains loggable"),
            std::string::npos);
}

TEST_F(Logging, InitializationFailureDoesNotChangeBackendFailureOrCreateStorage) {
  const auto blocked = base / "blocked";
  std::ofstream(blocked) << "preserve";
  testing::internal::CaptureStderr();
  const auto initialized = logging::initialize(logging::ProcessRole::Backend, blocked);
  const auto stderr_output = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(initialized);
  EXPECT_TRUE(stderr_output.empty());
  EXPECT_NO_THROW(SPDLOG_ERROR("discarded-without-file"));
  folio::application::internal::BackendContext context;
  context.startup_paths = folio::config::StoragePaths{};
  EXPECT_EQ(folio::application::internal::Bootstrap(context),
            folio::application::internal::BackendActionOutcome::Failure);
  EXPECT_EQ(context.diagnostics.size(), 1u);
  EXPECT_EQ(folio::application::internal::Finish(context),
            folio::application::internal::BackendActionOutcome::Success);
  EXPECT_EQ(read(blocked), "preserve");
  EXPECT_FALSE(fs::exists(base / "normal"));
}

#ifndef _WIN32
TEST_F(Logging, SymlinkLogRootIsRejected) {
  fs::create_directory(base / "target");
  fs::create_directory_symlink(base / "target", base / "alias");
  EXPECT_FALSE(logging::initialize(logging::ProcessRole::Backend, base / "alias"));
  EXPECT_TRUE(fs::is_empty(base / "target"));
}
#endif
} // namespace
