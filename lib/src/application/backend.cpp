#include <folio/application/backend.hpp>
#include "application/internal/process.hpp"
#include "application/internal/backend_state_machine.hpp"
#include "application/internal/backend_states.hpp"

#include <utility>

namespace folio::application {

core::Result<LaunchedProcess, ProcessError> launch_backend() {
  auto executable = internal::current_executable();
  if (auto* error = std::get_if<ProcessError>(&executable))
    return *error;
  return internal::launch_process(std::get<std::filesystem::path>(executable), {"--backend-process"});
}

int run_backend() {
  using State = internal::BackendStateMachine::State;
  internal::BackendContext context;

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

}  // namespace folio::application
