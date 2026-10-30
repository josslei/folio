#pragma once

#include "application/process.hpp"
#include "storage/filesystem.hpp"

namespace folio::application::internal {

[[nodiscard]] core::internal::Result<LaunchedProcess, ProcessError>
launch_backend();

inline const std::filesystem::path backend_process_argument{"--backend-process"};
// Configuration loading is deferred. Production dispatch reports unavailable
// without resolving real user paths or initializing any Storage lifecycle.
int run_backend();

inline constexpr int backend_already_owned_exit = 4;
inline constexpr int backend_service_unavailable_exit = 3;
// Phase 4.1a acquires ownership and immediately releases it with a distinct
// unavailable outcome. No Catalog, Marker, readiness or service loop exists yet.
int run_backend(const core::storage::StoragePaths& paths);

}  // namespace folio::application::internal
