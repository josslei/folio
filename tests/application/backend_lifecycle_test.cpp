#include "application/internal/backend_state_machine.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace folio::application::internal {
namespace {
using State = BackendStateMachine::State;
using Outcome = BackendActionOutcome;

constexpr BackendStateMachine::TransitionTable transitions{{
    {State::Bootstrap, State::Serve, State::Report},
    {State::Serve, State::Stopping, State::Report},
    {State::Stopping, State::Report, State::Report},
    {State::Report, State::Finish, State::Finish},
}};

BackendStateMachine::Handlers handlers(std::function<Outcome(State)> action) {
  return {{
      [=] { return action(State::Bootstrap); },
      [=] { return action(State::Serve); },
      [=] { return action(State::Stopping); },
      [=] { return action(State::Report); },
      [=] { return action(State::Finish); },
  }};
}

TEST(BackendLifecycle, SuccessfulActionsFollowDefinedOrder) {
  std::vector<State> visited;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    return Outcome::Success;
  }), [](State, std::exception_ptr) { ADD_FAILURE() << "Unexpected exception"; });
  machine.run();
  EXPECT_EQ(visited, (std::vector<State>{State::Bootstrap, State::Serve,
      State::Stopping, State::Report, State::Finish}));
  machine.run();
  EXPECT_EQ(visited.size(), 5u);
}

TEST(BackendLifecycle, StartupFailureReportsThenExecutesFinish) {
  std::vector<State> visited;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    return state == State::Bootstrap ? Outcome::Failure : Outcome::Success;
  }), [](State, std::exception_ptr) { ADD_FAILURE() << "Unexpected exception"; });
  machine.run();
  EXPECT_EQ(visited, (std::vector<State>{State::Bootstrap, State::Report, State::Finish}));
}

TEST(BackendLifecycle, ServeFailureBypassesStoppingAndUsesSharedReport) {
  std::vector<State> visited;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    return state == State::Serve ? Outcome::Failure : Outcome::Success;
  }), [](State, std::exception_ptr) { ADD_FAILURE() << "Unexpected exception"; });
  machine.run();
  EXPECT_EQ(visited, (std::vector<State>{State::Bootstrap, State::Serve,
      State::Report, State::Finish}));
}

TEST(BackendLifecycle, StoppingAndReportFailuresCannotSkipFinish) {
  std::vector<State> visited;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    return state == State::Bootstrap || state == State::Serve
               ? Outcome::Success : Outcome::Failure;
  }), [](State, std::exception_ptr) { ADD_FAILURE() << "Unexpected exception"; });
  machine.run();
  EXPECT_EQ(visited, (std::vector<State>{State::Bootstrap, State::Serve,
      State::Stopping, State::Report, State::Finish}));
  machine.run();
  EXPECT_EQ(std::count(visited.begin(), visited.end(), State::Finish), 1);
}

TEST(BackendLifecycle, ExceptionsInServeAndReportStillExecuteFinish) {
  std::vector<State> visited;
  std::vector<std::string> errors;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    if (state == State::Serve || state == State::Report) {
      throw std::runtime_error("Injected lifecycle action failure");
    }
    return Outcome::Success;
  }), [&](State, std::exception_ptr error) {
    try { std::rethrow_exception(error); }
    catch (const std::exception& exception) { errors.push_back(exception.what()); }
  });
  machine.run();
  EXPECT_EQ(visited, (std::vector<State>{State::Bootstrap, State::Serve,
      State::Report, State::Finish}));
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_NE(errors.front().find("Injected lifecycle action failure"), std::string::npos);
}

TEST(BackendLifecycle, ThrowingErrorNotificationCannotBypassFinish) {
  std::vector<State> visited;
  BackendStateMachine machine(transitions, handlers([&](State state) {
    visited.push_back(state);
    if (state == State::Serve || state == State::Report) {
      throw std::runtime_error("primary action exception");
    }
    return Outcome::Success;
  }), [](State, std::exception_ptr) {
    throw std::runtime_error("secondary notification exception");
  });
  EXPECT_NO_THROW(machine.run());
  EXPECT_EQ(visited.back(), State::Finish);
  ASSERT_EQ(machine.failed_notifications().size(), 2u);
  for (const auto& failure : machine.failed_notifications()) {
    try { std::rethrow_exception(failure.original); }
    catch (const std::exception& error) {
      EXPECT_STREQ(error.what(), "primary action exception");
    }
    try { std::rethrow_exception(failure.notification_error); }
    catch (const std::exception& error) {
      EXPECT_STREQ(error.what(), "secondary notification exception");
    }
  }
}

TEST(BackendLifecycle, InvalidRegistrationIsRejectedBeforeAnyActionRuns) {
  int called = 0;
  auto actions = handlers([&](State) {
    ++called;
    return Outcome::Success;
  });
  auto invalid = transitions;
  invalid[0].source = State::Serve;
  EXPECT_THROW((BackendStateMachine(invalid, actions, [](State, std::exception_ptr) {})),
               std::invalid_argument);
  actions[4] = {};
  EXPECT_THROW((BackendStateMachine(transitions, actions, [](State, std::exception_ptr) {})),
               std::invalid_argument);
  EXPECT_EQ(called, 0);
}
}  // namespace
}  // namespace folio::application::internal
