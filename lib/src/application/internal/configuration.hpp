#pragma once

#include <folio/application/ipc.hpp>
#include <folio/application/backend.hpp>
#include "storage/filesystem.hpp"

namespace folio::application::internal {
// Pure resolution; does not create directories. Runtime may be unavailable.
core::Result<core::storage::ApplicationPaths, ProcessError>
application_paths();
// Minimal bootstrap only. TOML requires absolute normal_root and vault_root.
// Injected effective application paths keep tests away from real user storage.
core::Result<core::storage::StoragePaths, ProcessError> load_configuration(
    const core::storage::ApplicationPaths& application,
    const std::filesystem::path& file);
core::Result<core::storage::StoragePaths, ProcessError> load_configuration();
}  // namespace folio::application::internal

namespace folio::application::internal {
// Pure bootstrap resource address; creating/listening requires ownership.
core::Result<std::filesystem::path, IpcError> ipc_endpoint(
    const core::storage::StoragePaths& paths);
core::Result<ConnectionSettings, IpcError> connection_settings(
    const core::storage::StoragePaths& paths);
}  // namespace folio::application::internal
