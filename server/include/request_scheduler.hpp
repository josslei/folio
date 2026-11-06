#pragma once

#include <folio/ipc/ipc.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <exception>
#include <mutex>
#include <thread>

namespace folio::application::internal {

// Session-only scheduling, not Recovery or a persistent receipt. The executor
// owns all operation execution; connection workers only admit and deliver.
class RequestScheduler {
 public:
  using Result = core::Result<Message, IpcError>;
  using Action = std::function<Result()>;
  enum class ExecutionStage { Queued, Executing, Finished };
  struct Ticket {
    // A signal, not cancellation: publishing a final result wakes its connection.
    std::stop_source completed;
    std::mutex mutex;
    ExecutionStage stage = ExecutionStage::Queued;
    std::optional<Result> result;
    std::exception_ptr exception;
  };

  explicit RequestScheduler(std::size_t capacity);
  ~RequestScheduler();
  RequestScheduler(const RequestScheduler&) = delete;
  // nullptr means rejected before acceptance. Capacity includes the active job.
  std::shared_ptr<Ticket> admit(Action action);
  void stop_admission();
  // Drains every accepted action, including actions whose connection is gone.
  // Action exceptions are retained on their tickets, never rethrown by drain;
  // failed actions do not abandon subsequent accepted work. Return means the
  // executor has joined, not that operations succeeded. Idempotent; lifecycle
  // thread only, never call from an action. Must complete before finalization.
  // Not noexcept: synchronization/thread infrastructure failures are distinct
  // from action failures and must not silently certify completed draining.
  void drain();

 private:
  struct Work { Action action; std::shared_ptr<Ticket> ticket; };
  void execute();
  const std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Work> queue_;
  std::size_t outstanding_ = 0;
  bool accepting_ = true;
  std::thread executor_;
};

}  // namespace folio::application::internal
