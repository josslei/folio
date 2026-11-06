#pragma once

#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <vector>
#include <string>

namespace folio::application::internal {

enum class BackendActionOutcome { Success, Failure };

// Owns lifecycle control only. Registered handlers return outcomes; they cannot
// inspect or change its private state. Finish always executes before termination.
class BackendStateMachine {
 public:
  enum class State { Bootstrap, Serve, Stopping, Report, Finish };
  struct Transition {
    State source;
    State success;
    State failure;
  };
  using TransitionTable = std::array<Transition, 4>;
  // Handler slots follow State declaration order, including terminal Finish.
  using Handlers = std::array<std::function<BackendActionOutcome()>, 5>;
  using ErrorHandler = std::function<void(State, std::exception_ptr)>;
  struct FailedNotification {
    State phase;
    std::exception_ptr original;
    std::exception_ptr notification_error;
    std::chrono::steady_clock::time_point original_time;
    std::chrono::steady_clock::time_point notification_time;
  };

  BackendStateMachine(TransitionTable transitions, Handlers handlers,
                      ErrorHandler on_error);
  void run();
  // If notifying the owner throws, retain both exceptions and continue cleanup.
  const std::vector<FailedNotification>& failed_notifications() const noexcept;


 private:
  TransitionTable transitions_;
  Handlers handlers_;
  ErrorHandler on_error_;
  State state_ = State::Bootstrap;
  bool completed_ = false;
  std::vector<FailedNotification> failed_notifications_;
};

}  // namespace folio::application::internal
