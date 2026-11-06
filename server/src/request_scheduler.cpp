#include "request_scheduler.hpp"

namespace folio::application::internal {

RequestScheduler::RequestScheduler(std::size_t capacity)
    : capacity_(capacity), executor_([this] { execute(); }) {}

RequestScheduler::~RequestScheduler() { drain(); }

std::shared_ptr<RequestScheduler::Ticket> RequestScheduler::admit(Action action) {
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

void RequestScheduler::stop_admission() {
  {
    std::lock_guard lock(mutex_);
    accepting_ = false;
  }
  changed_.notify_one();
}

void RequestScheduler::drain() {
  stop_admission();
  if (executor_.joinable()) executor_.join();
}

void RequestScheduler::execute() {
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
    work.ticket->completed.request_stop();
    {
      std::lock_guard lock(mutex_);
      --outstanding_;
    }
  }
}

}  // namespace folio::application::internal
