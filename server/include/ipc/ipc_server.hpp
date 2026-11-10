#pragma once

#include <folio/ipc/application_protocol.hpp>
#include "runtime/repository_ownership.hpp"
#include "operations/request_scheduler.hpp"
#include "ipc/connection_session.hpp"

#include <atomic>

namespace folio::application::internal {

struct RuntimeLimits {
  std::size_t connections = 64;
  std::size_t requests = 32;
  // Reserve one maximum-sized exchange before buffering each Request. Holding
  // this reservation through delivery also bounds queued requests and results.
  std::size_t buffered_requests = 4;
};

// The Backend session owns one IpcServer for all Frontend connections and one
// shared RequestScheduler. Connection workers admit/deliver; its executor runs
// handlers. Storage is accessed only through the executor handler, never by a
// connection worker.
// No additional lifecycle state machine: Serve runs this resource; Stopping and
// Report preserves usable connections; Finish closes them before Catalog finalization.
class IpcServer {
public:
  using Failure = std::variant<IpcError, std::exception_ptr>;
  using Handler = std::function<OperationResponse(const OperationRequest&, const Session&)>;
  IpcServer(RepositoryOwnership& ownership, Handler handler, std::string startup_failure = {},
            RuntimeLimits limits = {});
  ~IpcServer();
  IpcServer(const IpcServer&) = delete;
  /// Blocks the lifecycle thread while accepting connections; workers handle IPC
  /// and the constructor-started scheduler executes handlers independently.
  /// Returning stops admission, but does not certify draining or join all workers.
  /// Stopping/Report drain; Finish closes connections before Storage finalization.
  core::Result<std::monostate, IpcError> run(std::stop_token shutdown = {});
  // Stop new connections/requests without closing established connections.
  void stop_admission();
  // Attempt every accepted action and join the executor. Idempotent; retains
  // usable connections for Report. Operation errors/exceptions remain collected
  // outcomes; they do not escape drain or abandon the queue. This is not a
  // guarantee against broken synchronization infrastructure or a hung action.
  // Does not certify a clean Catalog session.
  void drain_requests();
  // After draining, wake and join connection workers, releasing their streams.
  // Finish calls this before Catalog finalization; it performs no Catalog work.
  void close_connections();
  // Transfer retained failures to the lifecycle owner and empty the collection.
  // Does not log, deliver, or resolve them. Later failures can be taken again.
  std::vector<Failure> take_failures();

private:
  struct Connection {
    std::jthread worker;
    bool done = false;
    bool busy = false;
  };
  /// Runs on this connection's worker: handshake, receive/validate, admit,
  /// acknowledge acceptance, await completion and deliver the final response.
  void connection(Connection& connection, IpcStream& stream, std::stop_token stop);
  /// Caller holds connection_state_mutex_; wakes the lifecycle-thread accept loop.
  void notify();
  void failure(Failure error);
  bool send_rejection(IpcStream& stream, ApplicationError error, std::stop_token stop);
  struct BufferReservation {
    explicit BufferReservation(std::atomic<std::size_t>& count) : count(count) {}
    ~BufferReservation() { --count; }
    std::atomic<std::size_t>& count;
  };
  std::shared_ptr<BufferReservation> reserve_request();

  RepositoryOwnership& ownership_;
  Handler handler_;
  const std::string startup_failure_;
  const RuntimeLimits limits_;
  RequestScheduler scheduler_;
  /// Protects connection done/busy flags, active_/served_/stopping_, changed_ and
  /// failures_. Registration, last-disconnect detection and admission closure
  /// form one invariant: splitting their locks would permit shutdown races.
  /// The lifecycle thread alone changes the connections_ vector structure.
  /// Lock order: connection_state_mutex_ -> scheduler's queue mutex. Scheduler
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
  std::vector<Failure> failures_;
  std::atomic<std::size_t> reservations_{0};
};

} // namespace folio::application::internal
