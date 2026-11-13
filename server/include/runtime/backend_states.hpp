#pragma once

#include "runtime/backend_state_machine.hpp"
#include "ipc/ipc_server.hpp"
#include "operations/mutation_orchestrator.hpp"
#include "operations/access_orchestrator.hpp"
#include <folio/ipc/ipc.hpp>
#include "runtime/repository_ownership.hpp"
#include "storage/catalog_coordinator.hpp"
#include "storage/normal_repository.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <variant>
#include <optional>
#include <string>
#include <vector>

namespace folio::application::internal {
namespace storage = core::storage;

// The first diagnostic is the primary failure; subsequent entries retain
// reporting/finalization failures in occurrence order. Only presentation formats
// these errors as text. No public output or IPC contract is implied.
struct BackendDiagnostic {
  BackendStateMachine::State phase;
  std::variant<folio::config::ConfigurationError, ProcessError, IpcError, storage::CoordinatorError,
               std::exception_ptr>
      error;
  std::chrono::steady_clock::time_point occurred_at = std::chrono::steady_clock::now();
};

std::string diagnostic_message(const BackendDiagnostic& diagnostic);

// Shared resources for handlers; contains no lifecycle control logic.
struct BackendContext {
  /// Outlives all worker resources and injected reporting callbacks.
  FatalErrorReporter fatal_errors;
  // Explicit internal bootstrap input; absent in normal executable startup.
  std::optional<config::StoragePaths> startup_paths;
  std::optional<storage::StoragePaths> paths;
  std::optional<RepositoryOwnership> ownership;
  storage::MetadataCatalog catalog;
  std::unique_ptr<storage::NormalRepository> repository;
  std::unique_ptr<storage::CatalogCoordinator> coordinator;
  std::unique_ptr<AccessOrchestrator> access;
  std::unique_ptr<MutationOrchestrator> mutations;
  std::unique_ptr<RequestCoordinator> request_coordinator;
  std::unique_ptr<IpcServer> runtime;
  // Historical obligation: startup_load succeeded. Current consistency remains
  // Coordinator-owned; finish_session itself rejects unsafe Catalog states.
  std::optional<RuntimeLimits> runtime_limits;
  bool initialized = false;
  int exit_code = 0;
  std::vector<BackendDiagnostic> diagnostics;
};

void retain_backend_notification_failures(BackendContext& context,
                                          const BackendStateMachine& machine);
/// Initializes configuration, ownership and Storage before admitting operations.
BackendActionOutcome Bootstrap(BackendContext& context);
/// Runs IPC acceptance on the lifecycle thread; prepared handlers run on coordinated execution
/// workers.
BackendActionOutcome Serve(BackendContext& context);
/// Normal-stop path: closes admission and drains accepted work, retaining streams.
BackendActionOutcome Stopping(BackendContext& context);
/// Also drains when failures bypass Stopping; retains diagnostics and usable streams.
/// Sends sanitized fatal shared failures via bounded Push delivery before Finish.
BackendActionOutcome Report(BackendContext& context);
/// Settles workers before eligible Catalog finalization, then releases ownership.
/// Unsafe Catalog state retains the Session Marker rather than certifying success.
BackendActionOutcome Finish(BackendContext& context);
BackendActionOutcome record_backend_failure(BackendContext& context, BackendDiagnostic diagnostic);

int run_backend(std::optional<config::StoragePaths> startup_paths);

// Registered once; the context must outlive the resulting handlers.
BackendStateMachine::Handlers register_backend_states(BackendContext& context);

} // namespace folio::application::internal
