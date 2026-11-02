#pragma once

#include <folio/application/ipc.hpp>
#include <folio/application/backend.hpp>
#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>
#include <functional>
#include <utility>

namespace folio::application {
// Owns an established Backend connection for TUI, CLI, and other Frontends.
// Destruction closes the connection; moving transfers ownership.
// Read access uses synchronous requests; one caller owns request/response access.
// No automatic
// reconnect/replay after handshake. One caller owns request/response access.
class BackendConnectionHandle {
 public:
  BackendConnectionHandle(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle& operator=(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle(const BackendConnectionHandle&) = delete;

  bool connected() const {
    return stream_.connected();
  }

  core::Result<std::monostate, internal::IpcError> ping();

  ApplicationResult<ApplicationResponse<BrowseResult>> browse(
      DirectoryRef dir, RecordOrder order = {});
  ApplicationResult<ApplicationResponse<std::vector<core::RecordMetadata>>> query_records(
      const RecordQuery& query = {});
  ApplicationResult<ApplicationResponse<std::vector<core::DirectoryMetadata>>> list_directories(
      core::DirectoryKind kind);
  ApplicationResult<core::Log::Snapshot> load_log(const core::Id& id);
  ApplicationResult<core::Memo::Snapshot> load_memo(const core::Id& id);
  ApplicationResult<core::Todo::Snapshot> load_todo(const core::Id& id);
  static core::Result<BackendConnectionHandle, internal::IpcError> connect(
      const internal::ConnectionSettings& paths,
      std::chrono::milliseconds timeout = std::chrono::seconds(3));

 private:
  explicit BackendConnectionHandle(internal::IpcStream stream) : stream_(std::move(stream)) {
  }

  internal::IpcStream stream_;
};

// Injectable launch operation permits controlled process tests without private
// production CLI path arguments. Production uses launch_backend().
using BackendLauncher = std::function<core::Result<LaunchedProcess, ProcessError>()>;

core::Result<BackendConnectionHandle, internal::IpcError> connect_or_start_backend(
    const internal::ConnectionSettings& paths, const BackendLauncher& launch,
    std::chrono::milliseconds timeout = std::chrono::seconds(15));

core::Result<BackendConnectionHandle, internal::IpcError> connect_or_start_backend();

}  // namespace folio::application
