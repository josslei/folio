#pragma once

#include <folio/ipc/application_protocol.hpp>
#include "runtime/repository_ownership.hpp"
#include "execution/request_coordinator.hpp"
#include "ipc/connection_session.hpp"

#include "runtime/fatal_error_reporter.hpp"
#include <atomic>
#include <map>

namespace folio::application::internal {

struct RuntimeLimits {
  // Logical session limit; at most two physical connection workers per slot.
  std::size_t connections;
  std::size_t requests;
  // Reserve one maximum-sized exchange before buffering each Request. Holding
  // this reservation through delivery also bounds queued requests and results.
  std::size_t buffered_requests;
  std::size_t access_workers;
  std::size_t pending_push_notification_capacity;
  std::chrono::milliseconds push_timeout;
  /// Values come from a validated startup configuration; no local defaults.
  explicit RuntimeLimits(const config::EffectiveConfig& config)
      : connections(config.backend_sessions), requests(config.backend_requests),
        buffered_requests(config.backend_buffered_requests), access_workers(config.access_workers),
        pending_push_notification_capacity(config.backend_push_queue_capacity),
        push_timeout(config.push_ack_timeout) {}
};

// BackendContext owns one IpcServer and one peer RequestCoordinator. IpcServer
// borrows scheduling and fatal reporting, and owns only connection workers. Connection workers
// admit/deliver; coordinated workers run prepared handlers. Storage is never accessed by a
// connection worker. Generic handlers without a preparer remain serialized. No additional lifecycle
// state machine: Serve runs this resource; Stopping and Report preserves usable connections; Finish
// closes them before Catalog finalization.
class IpcServer {
public:
  /// Borrows Backend-owned scheduling and fatal reporting; never starts execution workers.
  IpcServer(RequestCoordinator& request_coordinator, FatalErrorReporter& fatal_errors,
            RepositoryOwnership& ownership, std::string startup_failure, RuntimeLimits limits);
  ~IpcServer();
  IpcServer(const IpcServer&) = delete;
  /// Blocks the lifecycle thread while accepting connections; workers handle IPC
  /// and RequestCoordinator dispatches prepared work independently.
  /// Returning stops admission, but does not certify draining or join all workers.
  /// Stopping/Report drain; Finish closes connections before Storage finalization.
  core::Result<std::monostate, IpcError> run(std::stop_token shutdown = {});
  // Stop new connections/requests without closing established connections.
  void stop_admission();
  /// Lifecycle thread only, after the coordinator drains: wait for final-response
  /// delivery and reap disconnected workers. Does not drain execution itself.
  void wait_for_responses();
  // After draining, wake and join connection workers, releasing their streams.
  // Finish calls this before Catalog finalization; it performs no Catalog work.
  void close_connections();
  /// Broadcast small sanitized Push notifications, ordered under the publication caller's boundary.
  /// Never performs I/O; an overflowing recipient fails independently.
  void publish(const PushNotification& notification);
  /// Report-stage barrier after draining producers: waits for queued Push ACKs or
  /// session failure, bounded by the configured delivery timeout for the whole barrier.
  /// Returns false if any recipient fails or times out; timed-out sessions are ended.
  bool wait_for_push_delivery();

private:
  struct PushDelivery;
  struct Connection {
    Session session;
    std::shared_ptr<PushDelivery> delivery;
    std::jthread worker;
    bool done = false;
    bool busy = false;
  };
  /// Worker entry point for both connection roles: validate Hello, then dispatch.
  void serve_connection(Connection& connection, IpcStream& stream, std::stop_token stop);
  /// Request-role handler; called only after serve_connection validates RequestHello.
  /// Registers the session, admits requests and delivers their final responses.
  void serve_request_connection(Connection& connection, IpcStream& stream, std::stop_token stop);
  /// Caller holds connection_state_mutex_; wakes the lifecycle-thread accept loop.
  void notify();
  /// Push Notification-role handler; called only after validated session pairing.
  /// Establish coverage, deliver in order and validate receipts.
  /// Holds no Storage references and never waits on operation execution.
  void serve_push_notification_connection(IpcStream& stream, std::shared_ptr<PushDelivery> delivery,
                                          std::stop_token stop);
  static RequestCoordinator::Result encode_response(FatalErrorReporter& fatal_errors,
                                                    OperationResponse response, bool mutation);
  bool send_rejection(IpcStream& stream, ApplicationError error, std::stop_token stop);
  struct BufferReservation {
    explicit BufferReservation(std::shared_ptr<std::atomic<std::size_t>> count)
        : count(std::move(count)) {}
    ~BufferReservation() { --*count; }
    std::shared_ptr<std::atomic<std::size_t>> count;
  };
  std::shared_ptr<BufferReservation> reserve_request();

  /// Protects session pairing and broadcast order, never held during I/O or joins.
  std::mutex sessions_mutex_;
  /// Single-use pairing capabilities; removed when the Push connection attaches.
  std::map<std::string, std::weak_ptr<PushDelivery>> pending_push_pairings_;
  std::vector<std::weak_ptr<PushDelivery>> recipients_;
  FatalErrorReporter& fatal_error_reporter_;
  RepositoryOwnership& ownership_;
  const std::string startup_failure_;
  const RuntimeLimits limits_;
  RequestCoordinator& request_coordinator_;
  /// Protects connection done/busy flags, active_/served_/stopping_, changed_. Registration,
  /// last-disconnect detection and admission closure form one invariant: splitting their locks
  /// would permit shutdown races. The lifecycle thread alone changes the connections_ vector
  /// structure. Lock order: connection_state_mutex_ -> RequestCoordinator's queue mutex. Its
  /// actions run without its queue mutex and may then report failures here.
  /// Never execute handlers, perform blocking stream reads/writes, drain or join
  /// with this lock held.
  /// Condition-variable waits release it while sleeping.
  std::mutex connection_state_mutex_;
  std::condition_variable workers_changed_;
  std::stop_source changed_;
  std::stop_source admission_closed_;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::size_t active_ = 0;
  bool served_ = false, stopping_ = false;
  /// Accepted resource captures may outlive IPC teardown during partial cleanup.
  std::shared_ptr<std::atomic<std::size_t>> reservations_ =
      std::make_shared<std::atomic<std::size_t>>(0);
};

} // namespace folio::application::internal
