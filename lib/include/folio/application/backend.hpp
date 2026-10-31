#pragma once

#include <folio/core/result.hpp>

#include <cstdint>
#include <string>
#include <system_error>

namespace folio::application {

enum class ProcessErrorCode {
  InvalidArgument,
  IoError,
  AlreadyOwned,
  LaunchFailed
};

struct ProcessError {
  ProcessErrorCode code;
  std::string message;
  std::error_code system_error;
};

// Process creation only: neither ownership nor application readiness is implied.
// No OS handle is exposed. The ID is diagnostic, not a safe
// long-lived capability (OS process IDs may be reused).
struct LaunchedProcess {
  std::uint64_t id;
};

// Starts a separate folio process in Backend mode. Readiness is established by
// a subsequent connection handshake, not by successful process creation.
[[nodiscard]] core::Result<LaunchedProcess, ProcessError> launch_backend();

// Resolves configuration, acquires ownership, and runs the Backend lifecycle.
// Exit code 4 indicates that another Backend already owns the Folio Repo.
int run_backend();

}  // namespace folio::application
