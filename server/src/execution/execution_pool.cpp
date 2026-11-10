#include "execution/execution_pool.hpp"

namespace folio::application::internal {
ExecutionPool::ExecutionPool(std::size_t workers) {
  try {
    for (std::size_t i = 0; i < workers; ++i)
      workers_.emplace_back([this] { run(); });
  } catch (...) {
    drain();
    throw;
  }
}
ExecutionPool::~ExecutionPool() {
  drain();
}
void ExecutionPool::submit(std::function<void()> task) {
  {
    std::lock_guard lock(task_state_mutex_);
    tasks_.push_back(std::move(task));
  }
  changed_.notify_one();
}
void ExecutionPool::drain() {
  {
    std::lock_guard lock(task_state_mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
}
void ExecutionPool::run() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(task_state_mutex_);
      changed_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}
} // namespace folio::application::internal
