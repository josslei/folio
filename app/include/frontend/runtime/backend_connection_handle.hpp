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
#include <vector>
#include "frontend/runtime/connection_session.hpp"

namespace folio::application {
using RecordMetadataList = std::vector<core::RecordMetadata>;
using DirectoryMetadataList = std::vector<core::DirectoryMetadata>;

/// Frontend request APIs and session ownership; blocking IPC runs on the Request Worker.
/// No UI state or UI dependencies.
/// Main-thread submit/drain; wakeup runs on communication threads and only signals.
class BackendConnectionHandle {
public:
  /// Callbacks run on the main thread during completion delivery.
  // Generic completion
  template <class T> using CompletionCallback = std::function<void(T)>;

  // Access operations
  template <class T> using AccessCallback = CompletionCallback<AccessResult<T>>;

  // Annotated access operations
  template <class T> using AnnotatedAccessCallback = AccessCallback<Annotated<T>>;

  // Mutation operations
  template <class T> using MutationCallback = CompletionCallback<MutationResult<T>>;

  BackendConnectionHandle(std::unique_ptr<ConnectionSession> session, std::function<void()> wakeup);
  ~BackendConnectionHandle();
  /// Main thread: finish local Request work before destroying completion targets.
  void finish_requests();
  /// Nonblocking typed requests. Return false when busy or stopped; no callback runs.
  /// Accepted callbacks run on the main thread during process_completion().
  /// Arguments are copied for worker execution; callback targets must outlive completion.
  bool ping(CompletionCallback<core::Result<std::monostate, internal::IpcError>> completed);
  bool browse(DirectoryRef dir, RecordOrder order, AnnotatedAccessCallback<BrowseResult> completed);
  bool query_records(const RecordQuery& query,
                     AnnotatedAccessCallback<RecordMetadataList> completed);
  bool list_directories(core::DirectoryKind kind,
                        AnnotatedAccessCallback<DirectoryMetadataList> completed);
  bool check_consistency(DirectoryRef scope, AccessCallback<ConsistencyCheckResult> completed);
  bool refresh(RefreshScope scope, AccessCallback<std::monostate> completed);
  bool load_log(const core::Id& id, AnnotatedAccessCallback<core::Log::Snapshot> completed);
  bool load_memo(const core::Id& id, AnnotatedAccessCallback<core::Memo::Snapshot> completed);
  bool load_todo(const core::Id& id, AnnotatedAccessCallback<core::Todo::Snapshot> completed);
  bool acquire_edit(const core::Id& id, AccessCallback<std::monostate> completed);
  bool release_edit(const core::Id& id, AccessCallback<std::monostate> completed);
  bool create(const MutationId& mutation_id, const LogEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              MutationCallback<core::LogMetadata> completed);
  bool create(const MutationId& mutation_id, const MemoEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              std::optional<core::Id> directory_id, MutationCallback<core::MemoMetadata> completed);
  bool create(const MutationId& mutation_id, const TodoEditableFields& fields,
              const core::TimestampContext& created, const core::TimestampContext& modified,
              std::optional<core::Id> directory_id, MutationCallback<core::TodoMetadata> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const LogEditableFields& fields, const core::TimestampContext& modified,
              MutationCallback<core::LogMetadata> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const MemoEditableFields& fields, const core::TimestampContext& modified,
              MutationCallback<core::MemoMetadata> completed);
  bool update(const MutationId& mutation_id, const core::Id& entry_id,
              const TodoEditableFields& fields, const core::TimestampContext& modified,
              MutationCallback<core::TodoMetadata> completed);
  bool move(const MutationId& mutation_id, const core::Id& entry_id,
            std::optional<core::Id> destination, const core::TimestampContext& modified,
            MutationCallback<core::RecordMetadata> completed);
  bool create(const MutationId& mutation_id, core::DirectoryKind kind, const std::string& name,
              MutationCallback<core::DirectoryMetadata> completed);
  bool rename_directory(const MutationId& mutation_id, const core::Id& entry_id,
                        const std::string& new_name,
                        MutationCallback<core::DirectoryMetadata> completed);
  bool set_todo_status(const MutationId& mutation_id, const core::Id& entry_id,
                       core::TodoStatus status, const core::TimestampContext& modified,
                       MutationCallback<core::TodoMetadata> completed);
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
