#include <folio/logging/logging.hpp>
#include "logging/internal/session_file_test_access.hpp"
#include <folio/io/filesystem.hpp>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#ifndef _WIN32  // POSIX (Linux and macOS)
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace folio::logging {
namespace fs = std::filesystem;
namespace {
// Initialization/shutdown belong to the process entry thread. Log calls use the
// synchronous thread-safe default logger; no worker or periodic flush is started.
std::optional<fs::path> session_file;

void logging_error(const std::string&) noexcept {
  // Drop diagnostic failures silently. Console output could disrupt an active
  // TUI, and IPC/UI notification is outside the logging facility.
}

void replace_default_logger(std::shared_ptr<spdlog::logger> logger) {
  const auto previous = spdlog::default_logger();
  spdlog::set_default_logger(std::move(logger));
  if (previous && previous->name() != spdlog::default_logger()->name())
    spdlog::drop(previous->name());
}

void silence_default_logger() {
  auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("disabled", sink);
  logger->set_level(spdlog::level::warn);
  logger->set_error_handler(logging_error);
  replace_default_logger(std::move(logger));
}

std::uint64_t process_id() {
#ifndef _WIN32
  return static_cast<std::uint64_t>(getpid());
#else
  return GetCurrentProcessId();
#endif
}
}  // namespace

std::optional<fs::path> internal::reserve_session_file(
    const fs::path& directory, std::chrono::system_clock::time_point timestamp,
    std::uint64_t pid) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      timestamp - seconds).count();
  const auto time = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifndef _WIN32
  if (!gmtime_r(&time, &utc)) return std::nullopt;
#else
  if (gmtime_s(&utc, &time) != 0) return std::nullopt;
#endif
  char stamp[32];
  if (!std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &utc))
    return std::nullopt;
  const auto prefix = fmt::format("{}.{:03}Z-pid{}", stamp, milliseconds, pid);
  for (unsigned collision = 0; collision < 1000; ++collision) {
    auto filename = prefix;
    if (collision) filename += "-" + std::to_string(collision);
    const auto path = directory / (filename + ".log");
    // Presence-only exclusive creation needs no staging file or directory lock.
    // Existing sessions are never overwritten, even with an identical clock/PID.
    auto created = io::create_empty_file_durable(path);
    if (std::holds_alternative<std::monostate>(created)) return path;
    const auto& error = std::get<io::FileError>(created);
    if (error.replacement_completed || error.system_error != std::errc::file_exists)
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<fs::path> initialize(ProcessRole role, const fs::path& root) noexcept {
  try {
    if (session_file) return session_file;
    const auto name = role == ProcessRole::Backend ? "backend" : "frontend";
    const auto directory = root / name;
    auto created = io::create_directories_durable(directory);
    if (std::holds_alternative<io::FileError>(created)) {
      silence_default_logger();
      return std::nullopt;
    }
    auto file = internal::reserve_session_file(
        directory, std::chrono::system_clock::now(), process_id());
    if (!file) {
      silence_default_logger();
      return std::nullopt;
    }
    // Append mode only: exclusive reservation above owns this fresh session.
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file->native(), false);
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        "%Y-%m-%dT%H:%M:%S.%eZ [%l] [%n] [%s:%# %!] %v",
        spdlog::pattern_time_type::utc));
    logger->set_level(spdlog::level::warn);
    logger->flush_on(spdlog::level::err);
    logger->set_error_handler(logging_error);
    replace_default_logger(std::move(logger));
    session_file = std::move(file);
    return session_file;
  } catch (...) {
    try { silence_default_logger(); } catch (...) {}
    return std::nullopt;
  }
}

std::optional<fs::path> initialize(ProcessRole role) noexcept {
  try {
    auto paths = config::application_paths();
    if (auto* resolved = std::get_if<config::ApplicationPaths>(&paths))
      return initialize(role, resolved->logs);
    silence_default_logger();
  } catch (...) {
    // Logging cannot make configuration resolution or application startup fail.
  }
  return std::nullopt;
}

void set_level(config::LogLevel level) noexcept {
  try {
    spdlog::level::level_enum severity = spdlog::level::warn;
    switch (level) {
      case config::LogLevel::Trace: severity = spdlog::level::trace; break;
      case config::LogLevel::Debug: severity = spdlog::level::debug; break;
      case config::LogLevel::Info: severity = spdlog::level::info; break;
      case config::LogLevel::Warn: severity = spdlog::level::warn; break;
      case config::LogLevel::Error: severity = spdlog::level::err; break;
      case config::LogLevel::Critical: severity = spdlog::level::critical; break;
    }
    spdlog::default_logger()->set_level(severity);
  } catch (...) {
    // Diagnostic failure must not affect application behavior or terminal output.
  }
}

void shutdown() noexcept {
  try {
    spdlog::default_logger()->flush();
  } catch (...) {
    // Diagnostic failure must not affect application behavior or terminal output.
  }
  try {
    silence_default_logger();
    session_file.reset();
  } catch (...) {
    // Diagnostic failure must not affect application behavior or terminal output.
  }
}
}  // namespace folio::logging
