#pragma once

// Phase 4.1a process/ownership support is restricted to desktop platforms.
#if defined(__linux__)
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#error "Folio Application process support requires Linux, macOS, or Windows"
#endif
#elif defined(_WIN32)
#else
#error "Folio Application process support requires Linux, macOS, or Windows"
#endif

#include "backend_launcher.hpp"

#include <filesystem>
#include <vector>

namespace folio::application::internal {

[[nodiscard]] core::Result<std::filesystem::path, ProcessError>
current_executable();

// Absolute executable, native Unicode arguments, no shell. Detached from terminal
// input and unrelated descriptors/handles. May outlive the caller; POSIX children
// are reparented for reaping, Windows process handles are closed after creation.
[[nodiscard]] core::Result<LaunchedProcess, ProcessError> launch_process(
    const std::filesystem::path& executable,
    const std::vector<std::filesystem::path>& arguments);

}  // namespace folio::application::internal
