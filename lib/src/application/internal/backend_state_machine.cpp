#include "application/internal/backend_state_machine.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace folio::application::internal {

BackendStateMachine::BackendStateMachine(TransitionTable transitions,
                                         Handlers handlers, ErrorHandler on_error)
    : transitions_(std::move(transitions)), handlers_(std::move(handlers)),
      on_error_(std::move(on_error)) {
  // Reject invalid registration before any handler acquires resources.
  for (const auto required : {State::Bootstrap, State::Serve, State::Stopping,
                             State::Report}) {
    if (std::count_if(transitions_.begin(), transitions_.end(), [&](const auto& row) {
          return row.source == required;
        }) != 1) {
      throw std::invalid_argument("Each nonterminal Backend state needs one transition row");
    }
  }
  auto valid_state = [](State state) {
    return static_cast<std::size_t>(state) <= static_cast<std::size_t>(State::Finish);
  };
  for (const auto& row : transitions_) {
    if (!valid_state(row.success) || !valid_state(row.failure)) {
      throw std::invalid_argument("Invalid Backend transition destination");
    }
  }
  if (!on_error_ || std::any_of(handlers_.begin(), handlers_.end(), [](const auto& action) {
        return !action;
      })) {
    throw std::invalid_argument("All five Backend handlers and an error handler are required");
  }
}

void BackendStateMachine::run() {
  while (!completed_) {
    auto outcome = BackendActionOutcome::Failure;
    try {
      outcome = handlers_[static_cast<std::size_t>(state_)]();
    } catch (...) {
      auto original = std::current_exception();
      const auto occurred_at = std::chrono::steady_clock::now();
      try {
        on_error_(state_, original);
      } catch (...) {
        failed_notifications_.push_back(
            {state_, original, std::current_exception(), occurred_at,
             std::chrono::steady_clock::now()});
      }
    }
    // Finish is an action, not just the condition that terminates this loop.
    if (state_ == State::Finish) {
      completed_ = true;
      break;
    }
    const auto& row = *std::find_if(transitions_.begin(), transitions_.end(), [&](const auto& row) {
      return row.source == state_;
    });
    state_ = outcome == BackendActionOutcome::Success ? row.success : row.failure;
  }
}

const std::vector<BackendStateMachine::FailedNotification>&
BackendStateMachine::failed_notifications() const noexcept {
  return failed_notifications_;
}

}  // namespace folio::application::internal
