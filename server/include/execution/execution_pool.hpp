#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace folio::application::internal {

/// Private bounded execution support. RequestCoordinator reserves capacity before
/// submission; tasks must contain their own exceptions. No operation policy lives here.
class ExecutionPool {
public:
  explicit ExecutionPool(std::size_t workers);
  ~ExecutionPool();
  ExecutionPool(const ExecutionPool&) = delete;
  /// Called by the coordination thread, with at most workers outstanding tasks.
  void submit(std::function<void()> task);
  /// Stop after submitted tasks finish; lifecycle thread only, idempotent.
  void drain();

private:
  void run();
  /// Protects tasks_ and stopping_; released before task execution and joining.
  std::mutex task_state_mutex_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::condition_variable changed_;

  // Thread handles are managed by construction/draining, not task_state_mutex_.
  std::vector<std::thread> workers_;
};
} // namespace folio::application::internal
