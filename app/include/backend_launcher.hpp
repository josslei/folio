#pragma once

#include <folio/application/process_error.hpp>
#include <folio/core/result.hpp>
#include <cstdint>

namespace folio::application {

// Process creation only: neither ownership nor application readiness is implied.
// No OS handle is exposed. The ID is diagnostic, not a safe
// long-lived capability (OS process IDs may be reused).
struct LaunchedProcess {
  std::uint64_t id;
};

/// Starts a separate folio process in Backend mode. Readiness is established by
/// a subsequent connection handshake, not by successful process creation.
[[nodiscard]] core::Result<LaunchedProcess, ProcessError> launch_backend();

} // namespace folio::application
