#pragma once

#include <folio/ipc/ipc.hpp>
#include "backend_launcher.hpp"
#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <folio/application/mutation.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>
#include <functional>
#include <utility>

namespace folio::application::internal {
// Frontend convenience: load configuration, then derive the shared IPC settings.
core::Result<ConnectionSettings, IpcError> connection_settings();
// Frontend implementation-facing error adapter; never forwards diagnostic text.
ApplicationError operation_connection_error(const IpcError& error);
}

namespace folio::application {
// Owns an established Backend connection for TUI, CLI, and other Frontends.
// Destruction closes the connection; moving transfers ownership.
// All operations are synchronous; one caller owns request/response access.
// No automatic reconnect/replay after handshake. Run blocking calls off the UI
// thread. Moving the handle retains its connection-scoped editing permissions.
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
  // Acquire ownership and load the authoritative state in one executor action.
  // Ownership lasts until release or connection/accepted-work lifetime ends.
  ApplicationResult<EditSnapshot> acquire_edit(const core::Id& id);
  ApplicationResult<std::monostate> release_edit(const core::Id& id);
  MutationResult<core::LogMetadata> create(
      const MutationId& mutation_id,
      const LogEditableFields& fields,
      const core::TimestampContext& created,
      const core::TimestampContext& modified);
  MutationResult<core::MemoMetadata> create(
      const MutationId& mutation_id,
      const MemoEditableFields& fields,
      const core::TimestampContext& created,
      const core::TimestampContext& modified,
      std::optional<core::Id> directory_id);
  MutationResult<core::TodoMetadata> create(
      const MutationId& mutation_id,
      const TodoEditableFields& fields,
      const core::TimestampContext& created,
      const core::TimestampContext& modified,
      std::optional<core::Id> directory_id);
  MutationResult<core::LogMetadata> update(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      const LogEditableFields& fields,
      const core::TimestampContext& modified);
  MutationResult<core::MemoMetadata> update(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      const MemoEditableFields& fields,
      const core::TimestampContext& modified);
  MutationResult<core::TodoMetadata> update(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      const TodoEditableFields& fields,
      const core::TimestampContext& modified);
  MutationResult<core::RecordMetadata> move(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      std::optional<core::Id> destination,
      const core::TimestampContext& modified);
  MutationResult<core::DirectoryMetadata> create(
      const MutationId& mutation_id,
      core::DirectoryKind kind,
      const std::string& name);
  MutationResult<core::DirectoryMetadata> rename_directory(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      const std::string& new_name);
  MutationResult<core::TodoMetadata> set_todo_status(
      const MutationId& mutation_id,
      const core::Id& entry_id,
      core::TodoStatus status,
      const core::TimestampContext& modified);
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
