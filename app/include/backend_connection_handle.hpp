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
} // namespace folio::application::internal

namespace folio::application {
/// Owns an established Backend connection for TUI, CLI, and other Frontends.
/// Destruction closes it; moving transfers connection-scoped editing permissions.
/// Calls block and must not overlap on one handle; run them off the UI thread.
/// There is no automatic reconnect/replay. Closing does not cancel accepted work.
/// See docs/contracts/ipc.md for admission and final-confirmation semantics.
class BackendConnectionHandle {
public:
  BackendConnectionHandle(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle& operator=(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle(const BackendConnectionHandle&) = delete;

  bool connected() const { return stream_.connected(); }

  core::Result<std::monostate, internal::IpcError> ping();

  /// Browses a logical Root (RecordType) or Directory UUID using Catalog metadata.
  /// Roots return immediate Records and Directories; Logs have no Directories.
  ApplicationResult<ApplicationResponse<BrowseResult>> browse(DirectoryRef dir,
                                                              RecordOrder order = {});
  ApplicationResult<ApplicationResponse<std::vector<core::RecordMetadata>>>
  query_records(const RecordQuery& query = {});
  ApplicationResult<ApplicationResponse<std::vector<core::DirectoryMetadata>>>
  list_directories(core::DirectoryKind kind);
  /// Loads complete authoritative content through the current Catalog locator.
  /// Typed loads reject mismatched types and stale locations without a full scan.
  /// Read-only loading does not acquire editing rights.
  ApplicationResult<core::Log::Snapshot> load_log(const core::Id& id);
  ApplicationResult<core::Memo::Snapshot> load_memo(const core::Id& id);
  ApplicationResult<core::Todo::Snapshot> load_todo(const core::Id& id);
  /// Acquires editing ownership using only in-memory Catalog/ownership state.
  /// Same-owner reacquisition succeeds; another owner returns LockConflict.
  /// Load separately after acquisition to obtain current authoritative content.
  /// A failed Load retains ownership until release or connection cleanup.
  /// Accepted work retains the session token through disconnection.
  ApplicationResult<std::monostate> acquire_edit(const core::Id& id);
  /// Releases this connection's editing ownership; cannot release another owner's.
  ApplicationResult<std::monostate> release_edit(const core::Id& id);
  /// Create overloads generate Entry UUIDs in the Backend. The Frontend supplies
  /// MutationId and timestamp contexts; success returns synchronized metadata.
  /// MutationResult distinguishes confirmed failure from missing confirmation.
  MutationResult<core::LogMetadata> create(const MutationId& mutation_id,
                                           const LogEditableFields& fields,
                                           const core::TimestampContext& created,
                                           const core::TimestampContext& modified);
  MutationResult<core::MemoMetadata> create(const MutationId& mutation_id,
                                            const MemoEditableFields& fields,
                                            const core::TimestampContext& created,
                                            const core::TimestampContext& modified,
                                            std::optional<core::Id> directory_id);
  MutationResult<core::TodoMetadata> create(const MutationId& mutation_id,
                                            const TodoEditableFields& fields,
                                            const core::TimestampContext& created,
                                            const core::TimestampContext& modified,
                                            std::optional<core::Id> directory_id);
  /// Update overloads require this connection's Edit Lock and replace all editable
  /// fields, preserving current identity, creation, privacy and membership.
  /// The supplied modified context is used unchanged, not as a version token.
  MutationResult<core::LogMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                           const LogEditableFields& fields,
                                           const core::TimestampContext& modified);
  MutationResult<core::MemoMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                            const MemoEditableFields& fields,
                                            const core::TimestampContext& modified);
  MutationResult<core::TodoMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                            const TodoEditableFields& fields,
                                            const core::TimestampContext& modified);
  /// Moves a Memo/Todo to its kind's Directory, or Root when destination is empty.
  /// Preserves current content; rejects another owner's lock. An unlocked Record
  /// may move without acquiring a long-lived lock. Compound failures retain evidence.
  MutationResult<core::RecordMetadata> move(const MutationId& mutation_id, const core::Id& entry_id,
                                            std::optional<core::Id> destination,
                                            const core::TimestampContext& modified);
  MutationResult<core::DirectoryMetadata> create(const MutationId& mutation_id,
                                                 core::DirectoryKind kind, const std::string& name);
  /// Immediate mutation: respects existing ownership without retaining a new lock.
  /// Renaming preserves UUID-based folder identity and member Record contents.
  MutationResult<core::DirectoryMetadata> rename_directory(const MutationId& mutation_id,
                                                           const core::Id& entry_id,
                                                           const std::string& new_name);
  /// Immediate status change; respects existing ownership and preserves other fields.
  MutationResult<core::TodoMetadata> set_todo_status(const MutationId& mutation_id,
                                                     const core::Id& entry_id,
                                                     core::TodoStatus status,
                                                     const core::TimestampContext& modified);
  static core::Result<BackendConnectionHandle, internal::IpcError>
  connect(const internal::ConnectionSettings& paths,
          std::chrono::milliseconds timeout = std::chrono::seconds(3));

private:
  explicit BackendConnectionHandle(internal::IpcStream stream) : stream_(std::move(stream)) {}

  internal::IpcStream stream_;
};

// Injectable launch operation permits controlled process tests without private
// production CLI path arguments. Production uses launch_backend().
using BackendLauncher = std::function<core::Result<LaunchedProcess, ProcessError>()>;

core::Result<BackendConnectionHandle, internal::IpcError>
connect_or_start_backend(const internal::ConnectionSettings& paths, const BackendLauncher& launch,
                         std::chrono::milliseconds timeout = std::chrono::seconds(15));

core::Result<BackendConnectionHandle, internal::IpcError> connect_or_start_backend();

} // namespace folio::application
