#include "execution/request_coordinator.hpp"

namespace folio::application::internal {

RequestCoordinator::RequestCoordinator(std::size_t capacity)
    : capacity_(capacity), executor_([this] { execute(); }) {}

RequestCoordinator::~RequestCoordinator() {
  drain();
}

std::shared_ptr<RequestCoordinator::Ticket> RequestCoordinator::admit(Action action) {
  // Called by connection workers: admission only, never execute the action here.
  auto ticket = std::make_shared<Ticket>();
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ || outstanding_ >= capacity_) return {};
    queue_.push_back({std::move(action), ticket});
    ++outstanding_;
  }
  changed_.notify_one();
  return ticket;
}

void RequestCoordinator::stop_admission() {
  {
    std::lock_guard lock(mutex_);
    accepting_ = false;
  }
  changed_.notify_one();
}

void RequestCoordinator::drain() {
  stop_admission();
  if (executor_.joinable()) executor_.join();
}

void RequestCoordinator::execute() {
  // Single executor thread: wait/dequeue, execute, then publish completion.
  for (;;) {
    Work work;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return !queue_.empty() || !accepting_; });
      if (queue_.empty()) return;
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    {
      std::lock_guard lock(work.ticket->mutex);
      work.ticket->stage = ExecutionStage::Executing;
    }
    Result result;
    std::exception_ptr exception;
    // Release queue/ticket locks before invoking handlers: they may report a
    // failure to IpcServer, whose admission path acquires server then queue locks.
    try {
      result = work.action();
    } catch (...) {
      // No authoritative outcome is fabricated. The owning runtime treats this
      // as a service failure and still drains accepted work before finalization.
      exception = std::current_exception();
      result = IpcError{IpcErrorCode::IoError, "Backend operation raised an unexpected exception"};
    }
    {
      std::lock_guard lock(work.ticket->mutex);
      work.ticket->exception = exception;
      work.ticket->result.emplace(std::move(result));
      work.ticket->stage = ExecutionStage::Finished;
    }
    // Publish all ticket fields before waking the connection. Disconnecting
    // affects delivery only: the accepted action has already run to completion.
    work.ticket->completed.request_stop();
    {
      std::lock_guard lock(mutex_);
      --outstanding_;
    }
  }
}

} // namespace folio::application::internal
