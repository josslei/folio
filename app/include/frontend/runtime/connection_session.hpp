#pragma once
#include <folio/ipc/ipc.hpp>
#include <folio/application/push_notification.hpp>
#include <condition_variable>
#include <optional>
#include <functional>
#include <mutex>
#include <thread>
#include <memory>
#include "frontend/backend_launcher.hpp"

namespace folio::application::internal {
// Load Frontend configuration and derive IPC settings.
core::Result<ConnectionSettings, IpcError> connection_settings();
ApplicationError operation_connection_error(const IpcError& error);
} // namespace folio::application::internal

namespace folio::application {
/// Pending communication work; several reasons may be signaled together.
struct FrontendWork {
  bool completion = false;
  bool push_notifications = false;
  bool session_changed = false;
};
/// One Frontend IPC session owns both streams and both execution workers.
/// BackendConnectionHandle supplies work to the Request Worker.
/// ACK is independent of event consumption and UI work.
class ConnectionSession {
public:
  /// Starts one dedicated receiver; the stream is transferred exclusively to it.
  explicit ConnectionSession(internal::IpcStream request, internal::IpcStream stream,
                             std::size_t capacity);
  ~ConnectionSession();
  static core::Result<std::unique_ptr<ConnectionSession>, internal::IpcError>
  connect(const internal::ConnectionSettings& settings,
          std::chrono::milliseconds timeout = std::chrono::seconds(3));
  /// Idempotent session termination; wakes receiver and consumer waits, never joins.
  void close();
  using Completion = std::function<void()>;
  /// Internal communication work; returned completion owns its result.
  using Request = std::function<Completion(ConnectionSession&)>;
  /// Start once; session and wakeup target outlive worker execution.
  void start_requests(std::function<void()> wakeup);
  /// Nonblocking submission; rejects an occupied or stopped single Request slot.
  bool submit(Request request);
  /// Main thread: join Request work and apply its completion before targets die.
  void finish_requests();
  /// Main thread: consume completion; rethrows worker/callback exceptions after
  /// closing the session, without fabricating an Application or Mutation result.
  void process_completion();
  /// Main thread, on Wakeup: consume reasons, preserving deferred Push work.
  FrontendWork take_pending_work();
  /// Internal Request path; used only on the owning Request Worker.
  internal::IpcStream& request_connection() { return request_worker_.stream; }

  /// Install a bounded wakeup signal; caller keeps its target alive.
  void set_wakeup(std::function<void()> wakeup);
  bool connected() const { return !ended_.stop_requested() && request_worker_.stream.connected(); }
  /// UI integration must apply the returned request result before draining events.
  /// Events received during a request remain retained; ACK is never deferred.
  std::vector<PushNotification> take();
  /// Event-driven integration wait, separate from Push I/O/ACK. false on close/stop.
  bool wait(std::stop_token stop = {});
  /// Synchronous Request owner brackets its call; no overlapping Application calls.
  void request_started();
  /// Releases retention after transport completion; UI must apply the result before draining.
  void request_finished();
  std::stop_token stop_token() const { return ended_.get_token(); }

private:
  /// Push Notification Worker: validate, retain, ACK, and signal without UI work.
  void receive_push_notifications();
  /// Snapshot the signal under lock; invoke outside every session mutex.
  void signal_wakeup() noexcept;
  /// Request path resources; connection and execution share one lifetime.
  struct RequestWorker {
    internal::IpcStream stream;
    // Protects the single Request/completion slot; never held for IPC/callbacks.
    std::mutex mutex;
    std::condition_variable_any changed;
    Request pending;
    Completion completion;
    std::exception_ptr failure;
    bool outstanding = false;
    bool completion_ready = false;
    bool deferred_push = false;
    std::jthread thread;
  };
  /// Push Notification path resources; no UI-state processing occurs here.
  struct PushNotificationWorker {
    internal::IpcStream stream;
    // Protects retained notices, request handoff and wakeup callback.
    std::mutex mutex;
    std::condition_variable_any changed;
    std::vector<std::optional<PushNotification>> pending;
    std::size_t pending_count = 0;
    std::shared_ptr<const std::function<void()>> wakeup;
    bool request_active = false;
    bool ready = false;
    bool session_changed = false;
    std::jthread thread;
  };
  const std::size_t capacity_;
  std::stop_source ended_;
  RequestWorker request_worker_;
  PushNotificationWorker push_notification_worker_;
};

using BackendLauncher = std::function<core::Result<LaunchedProcess, ProcessError>()>;
core::Result<std::unique_ptr<ConnectionSession>, internal::IpcError>
connect_or_start_backend(const internal::ConnectionSettings& paths, const BackendLauncher& launch,
                         std::chrono::milliseconds timeout = std::chrono::seconds(15));
core::Result<std::unique_ptr<ConnectionSession>, internal::IpcError> connect_or_start_backend();
} // namespace folio::application
