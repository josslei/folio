#pragma once

// Phase 4.1a process/ownership support is restricted to desktop platforms.
#if defined(_WIN32)
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#error "Folio Application process support requires macOS, Linux, or Windows"
#endif
#elif defined(__linux__)
#else
#error "Folio Application process support requires macOS, Linux, or Windows"
#endif

#include "core/internal/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace folio::application::internal {

enum class ProcessErrorCode { InvalidArgument, IoError, AlreadyOwned, LaunchFailed };
struct ProcessError {
  ProcessErrorCode code;
  std::string message;
  std::error_code system_error;
};

// Process creation only: neither ownership nor application readiness is implied.
// No OS handle escapes this internal utility. The ID is diagnostic, not a safe
// long-lived capability (OS process IDs may be reused).
struct LaunchedProcess { std::uint64_t id; };

[[nodiscard]] core::internal::Result<std::filesystem::path, ProcessError>
current_executable();

// Absolute executable, native Unicode arguments, no shell. Detached from terminal
// input and unrelated descriptors/handles. May outlive the caller; POSIX children
// are reparented for reaping, Windows process handles are closed after creation.
[[nodiscard]] core::internal::Result<LaunchedProcess, ProcessError> launch_process(
    const std::filesystem::path& executable,
    const std::vector<std::filesystem::path>& arguments);

}  // namespace folio::application::internal
