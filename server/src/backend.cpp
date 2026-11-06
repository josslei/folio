#include "backend.hpp"
#include "runtime/backend_state_machine.hpp"
#include "runtime/backend_states.hpp"

#include <utility>

namespace folio::application {

int internal::run_backend(std::optional<config::StoragePaths> startup_paths) {
  using State = internal::BackendStateMachine::State;
  internal::BackendContext context;
  context.startup_paths = std::move(startup_paths);

  // Source state        Success          Unrecoverable failure
  const internal::BackendStateMachine::TransitionTable transitions{{
      {State::Bootstrap, State::Serve,     State::Report},
      {State::Serve,     State::Stopping,  State::Report},
      {State::Stopping,  State::Report,    State::Report},
      {State::Report,    State::Finish,    State::Finish},
  }};
  internal::BackendStateMachine machine(
      transitions, internal::register_backend_states(context),
      [&](State phase, std::exception_ptr error) {
        internal::record_backend_failure(context, {phase, error});
      });
  machine.run();
  internal::retain_backend_notification_failures(context, machine);
  return context.exit_code;
}

int run_backend() {
  return internal::run_backend(std::nullopt);
}

}  // namespace folio::application
