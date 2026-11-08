#include "backend_launcher.hpp"
#include "process.hpp"

namespace folio::application {

core::Result<LaunchedProcess, ProcessError> launch_backend() {
  auto executable = internal::current_executable();
  if (auto* error = std::get_if<ProcessError>(&executable)) return *error;
  return internal::launch_process(std::get<std::filesystem::path>(executable),
                                  {"--backend-process"});
}

} // namespace folio::application
