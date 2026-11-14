#pragma once
#include <folio/ipc/ipc.hpp>

#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <folio/application/maintenance.hpp>
#include <folio/application/mutation.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>
#include <functional>
#include <folio/application/push_notification.hpp>
#include <memory>
#include <utility>
#include "frontend/runtime/connection_session.hpp"

namespace folio::application {
/// Frontend request APIs and session ownership; blocking IPC runs on the Request Worker.
/// No UI state or UI dependencies.
/// Main-thread submit/drain; wakeup runs on communication threads and only signals.
class BackendConnectionHandle {
public:
  BackendConnectionHandle(std::unique_ptr<ConnectionSession> session, std::function<void()> wakeup);
  ~BackendConnectionHandle();
  /// Main thread: finish local Request work before destroying completion targets.
  void finish_requests();
  /// Nonblocking typed requests. Return false when busy or stopped; no callback runs.
  /// Accepted callbacks run on the main thread during process_completion().
  /// Arguments are copied for worker execution; callback targets must outlive completion.
  bool ping(std::function<void(core::Result<std::monostate, internal::IpcError>)> completed);
  bool browse(DirectoryRef dir, RecordOrder order,
              std::function<void(AccessResult<Annotated<BrowseResult>>)> completed);
  bool query_records(
      const RecordQuery& query,
      std::function<void(AccessResult<Annotated<std::vector<core::RecordMetadata>>>)> completed);
  bool list_directories(
      core::DirectoryKind kind,
      std::function<void(AccessResult<Annotated<std::vector<core::DirectoryMetadata>>>)> completed);
  bool check_consistency(DirectoryRef scope,
                         std::function<void(AccessResult<ConsistencyCheckResult>)> completed);
  bool refresh(RefreshScope scope, std::function<void(AccessResult<std::monostate>)> completed);
  bool load_log(const core::Id& id,
                std::function<void(AccessResult<Annotated<core::Log::Snapshot>>)> completed);
  bool load_memo(const core::Id& id,
                 std::function<void(AccessResult<Annotated<core::Memo::Snapshot>>)> completed);
  bool load_todo(const core::Id& id,
                 std::function<void(AccessResult<Annotated<core::Todo::Snapshot>>)> completed);
  bool acquire_edit(const core::Id& id,
                    std::function<void(AccessResult<std::monostate>)> completed);
  bool release_edit(const core::Id& id,
                    std::function<void(AccessResult<std::monostate>)> completed);
  bool create(const MutationId& mutation_id, const LogEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              std::function<void(MutationResult<core::LogMetadata>)> completed);
  bool create(const MutationId& mutation_id, const MemoEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              std::optional<core::Id> directory_id,
              std::function<void(MutationResult<core::MemoMetadata>)> completed);
  bool create(const MutationId& mutation_id, const TodoEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              std::optional<core::Id> directory_id,
              std::function<void(MutationResult<core::TodoMetadata>)> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const LogEditableFields& fields, const core::TimestampContext& modified,
              std::function<void(MutationResult<core::LogMetadata>)> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const MemoEditableFields& fields, const core::TimestampContext& modified,
              std::function<void(MutationResult<core::MemoMetadata>)> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const TodoEditableFields& fields, const core::TimestampContext& modified,
              std::function<void(MutationResult<core::TodoMetadata>)> completed);
  bool move(const MutationId& mutation_id, const core::Id& entry_id,
            std::optional<core::Id> destination, const core::TimestampContext& modified,
            std::function<void(MutationResult<core::RecordMetadata>)> completed);
  bool create(const MutationId& mutation_id, core::DirectoryKind kind, const std::string& name,
              std::function<void(MutationResult<core::DirectoryMetadata>)> completed);
  bool rename_directory(const MutationId& mutation_id, const core::Id& entry_id,
                        const std::string& new_name,
                        std::function<void(MutationResult<core::DirectoryMetadata>)> completed);
  bool set_todo_status(const MutationId& mutation_id, const core::Id& entry_id,
                       core::TodoStatus status, const core::TimestampContext& modified,
                       std::function<void(MutationResult<core::TodoMetadata>)> completed);
  /// Main thread: apply completion before consuming retained Push Notifications.
  /// Rethrows Request/completion callback exceptions after session termination.
  void process_completion();
  std::vector<PushNotification> take_push_notifications();
  bool connected() const;
  /// Borrow transport state for integration; the handle retains ownership.
  ConnectionSession& connection_session() const { return *session_; }
  /// Called only for a communication Wakeup; atomically consumes its reasons.
  FrontendWork take_pending_work();

private:
  std::unique_ptr<ConnectionSession> session_;
};
} // namespace folio::application
