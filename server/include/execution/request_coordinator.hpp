#pragma once

#include <folio/ipc/ipc.hpp>
#include <folio/ipc/application_protocol.hpp>
#include "execution/execution_pool.hpp"
#include <array>
#include <map>
#include <optional>

#include <condition_variable>
#include <deque>
#include <functional>
#include <exception>
#include <mutex>
#include <thread>

namespace folio::application::internal {

/// Session-only admission and execution coordination, not Recovery or a receipt.
/// One short coordination thread dispatches bounded workers; IPC only admits/delivers.
class RequestCoordinator {
public:
  using Result = core::Result<Message, IpcError>;
  using Action = std::function<Result()>;
  using Preparation = std::function<Action()>;
  enum class Lane { Control, Query, Load, Mutation };
  struct Plan {
    Lane lane = Lane::Mutation;
    std::optional<std::string> entry;
  };
  /// Scheduling requirements only; no Catalog lookup or business validation.
  static Plan plan(const OperationRequest& request);
  enum class ExecutionStage { Queued, Executing, Finished };
  /// Shared completion state. Read stage/result/exception under mutex; the stop
  /// token is only a wakeup signal and does not itself grant access to the fields.
  struct Ticket {
    // A signal, not cancellation: publishing a final result wakes its connection.
    std::stop_source completed;
    /// Protects this ticket's stage, result and exception across the executor
    /// and connection worker. Release before executing actions or signaling
    /// completion; do not nest with the coordinator queue mutex.
    std::mutex mutex;
    ExecutionStage stage = ExecutionStage::Queued;
    std::optional<Result> result;
    std::exception_ptr exception;
  };

  /// Starts coordination, a shared bounded Access pool and one writer.
  /// access_workers must be positive; capacity counts preparation, queued and active work.
  /// Captures in accepted actions must stay valid until draining completes.
  explicit RequestCoordinator(std::size_t capacity, std::size_t access_workers = 4);
  ~RequestCoordinator();
  RequestCoordinator(const RequestCoordinator&) = delete;
  // nullptr means rejected before acceptance. Capacity includes the active job.
  std::shared_ptr<Ticket> admit(Action action);
  /// Access preparation and execution run on the same Access worker after reservation.
  /// Mutation protection is prepared on coordination before dispatch; Control stays there.
  /// Neither callback runs under queue/ticket locks. Accepted captures survive disconnect.
  std::shared_ptr<Ticket> admit(Plan plan, Preparation prepare);
  /// Thread-safe admission closure; wakes coordination without cancelling work.
  void stop_admission();
  // Drains every accepted action, including actions whose connection is gone.
  // Action exceptions are retained on their tickets, never rethrown by drain;
  // failed actions do not abandon subsequent accepted work. Return means the
  // coordination and workers have joined, not that operations succeeded. Idempotent; lifecycle
  // thread only, never call from an action. Must complete before finalization.
  // Not noexcept: synchronization/thread infrastructure failures are distinct
  // from action failures and must not silently certify completed draining.
  void drain();

private:
  struct Work {
    Plan plan;
    Preparation prepare;
    std::shared_ptr<Ticket> ticket;
  };
  /// Coordination thread: consume completions, reserve eligible work and prepare dispatch.
  void coordinate();
  /// Worker boundary: contain failures, release captures and enqueue bounded completion.
  void execute(Work work, Action action);
  /// Caller holds mutex_; reads the coordination-thread-owned reservation state.
  bool eligible(std::size_t index) const;
  /// Coordination thread only, with mutex_ held; no business ownership decisions.
  void reserve(const Work& work);
  /// Coordination thread only, with mutex_ held, after execution and protection cleanup.
  void release(const Work& work);
  const std::size_t capacity_;
  /// Protects queue_, completed_, outstanding_ and accepting_. IpcServer may enter admission
  /// while holding its connection-state lock; never call actions or IpcServer
  /// while holding this mutex. Ticket locks are acquired separately, not nested.
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Work> queue_;
  std::vector<Work> completed_;
  // The coordination thread alone owns lane counts and Entry reservations.
  std::array<std::size_t, 4> active_{};
  std::map<std::string, std::size_t> readers_;
  std::optional<std::string> writer_;
  const std::size_t access_workers_;
  ExecutionPool access_;
  ExecutionPool mutations_{1};
  std::size_t outstanding_ = 0;
  bool accepting_ = true;
  std::thread coordination_;
};

} // namespace folio::application::internal
