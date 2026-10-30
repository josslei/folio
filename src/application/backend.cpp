#include "application/backend.hpp"
#include "application/repository_ownership.hpp"

#include <iostream>

namespace folio::application::internal {
namespace fs = std::filesystem;
namespace storage = core::storage;
core::internal::Result<LaunchedProcess, ProcessError> launch_backend() {
  auto executable = current_executable();
  if (auto* error = std::get_if<ProcessError>(&executable)) return *error;
  return launch_process(std::get<fs::path>(executable), {backend_process_argument});
}

int run_backend() {
  std::cerr << "Backend configuration loading and connection service are not implemented\n";
  return backend_service_unavailable_exit;
}

int run_backend(const storage::StoragePaths& paths) {
  auto owner = RepositoryOwnership::acquire(paths);
  if (auto* error = std::get_if<ProcessError>(&owner)) {
    std::cerr << error->message << '\n';
    return error->code == ProcessErrorCode::AlreadyOwned ? backend_already_owned_exit : 1;
  }
  // Ownership works, but there is no service to keep alive until 4.1b. Never
  // claim Ready, initialize a Catalog or clear a Marker from this bootstrap.
  std::cerr << "Backend ownership acquired; connection service is not implemented\n";
  return backend_service_unavailable_exit;
}
}  // namespace folio::application::internal
