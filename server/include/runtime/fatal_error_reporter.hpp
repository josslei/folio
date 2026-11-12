#pragma once

#include <folio/ipc/ipc.hpp>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <variant>
#include <vector>

namespace folio::application::internal {
/// Backend-owned fatal-error inbox. Outlives IPC and accepted work using it.
/// Stores causes and signals the lifecycle waiter; performs no I/O, cancellation,
/// session closure or state transition. Ordinary Application errors do not belong here.
class FatalErrorReporter {
public:
  using Failure = std::variant<IpcError, std::exception_ptr>;
  /// Thread-safe submission; never rethrow the submitted operational exception.
  /// Allocation/synchronization exhaustion is not guaranteed recoverable.
  void report(Failure error);
  /// Capture before testing take_one(); reporting then wakes the returned token.
  /// A drained inbox starts a new wake generation, avoiding stale wakeup loops.
  std::stop_token wake_token();
  std::optional<Failure> take_one();
  /// Lifecycle owner drains remaining causes after execution has drained.
  std::vector<Failure> take_failures();

private:
  /// Protects retained failures and wake generation. No I/O/joins under this lock.
  std::mutex failures_mutex_;
  std::vector<Failure> failures_;
  std::stop_source changed_;
};
} // namespace folio::application::internal
