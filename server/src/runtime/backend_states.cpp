#include "runtime/backend_states.hpp"
#include "operations/access_orchestrator.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <folio/ipc/ipc.hpp>
#include <folio/config/configuration.hpp>
#include <folio/logging/logging.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

namespace folio::application::internal {
using Phase = BackendStateMachine::State;
BackendActionOutcome Bootstrap(BackendContext& context) {
  if (context.startup_paths) {
    auto validated = config::validate_effective_paths(*context.startup_paths);
    if (auto* error = std::get_if<config::ConfigurationError>(&validated)) {
      return record_backend_failure(context, {Phase::Bootstrap, *error});
    }
    context.paths = std::get<config::StoragePaths>(std::move(validated));
    auto defaults = config::configuration_defaults(context.paths->application);
    if (auto* error = std::get_if<config::ConfigurationError>(&defaults))
      return record_backend_failure(context, {Phase::Bootstrap, *error});
    context.runtime_limits.emplace(std::get<config::EffectiveConfig>(defaults));
  } else {
    auto resolved = config::load_configuration();
    if (auto* error = std::get_if<config::ConfigurationError>(&resolved)) {
      return record_backend_failure(context, {Phase::Bootstrap, *error});
    }
    auto effective = std::get<config::EffectiveConfig>(std::move(resolved));
    logging::set_level(effective.log_level);
    context.runtime_limits.emplace(effective);
    context.paths.emplace(std::move(effective.paths));
  }
  auto acquired = RepositoryOwnership::acquire(*context.paths);
  if (auto* error = std::get_if<ProcessError>(&acquired)) {
    if (error->code == ProcessErrorCode::AlreadyOwned) {
      context.exit_code = 4;
    }
    return record_backend_failure(context, {Phase::Bootstrap, *error});
  }
  context.ownership.emplace(std::get<RepositoryOwnership>(std::move(acquired)));
  context.repository = std::make_unique<storage::NormalRepository>(context.paths->normal_root);
  context.coordinator =
      std::make_unique<storage::CatalogCoordinator>(*context.paths, context.catalog);
  auto result = context.coordinator->startup_load();
  if (auto* error = std::get_if<storage::CoordinatorError>(&result)) {
    return record_backend_failure(context, {Phase::Bootstrap, *error});
  }
  const auto& outcome = std::get<storage::InitializationOutcome>(result);
  try {
    if (outcome.previous_session_incomplete)
      SPDLOG_WARN("Previous session incomplete; Catalog rebuilt from authoritative Entries");
    if (outcome.index_error)
      SPDLOG_WARN("Normal Index rejected; Catalog rebuilt (CoordinatorError code={})",
                  static_cast<int>(outcome.index_error->code));
    if (!outcome.diagnostics.empty())
      SPDLOG_WARN("Authoritative scan reported {} excluded filesystem objects",
                  outcome.diagnostics.size());
  } catch (...) {
    // Diagnostic failure cannot alter a successfully established session.
  }
  context.initialized = true;
  return BackendActionOutcome::Success;
}

BackendActionOutcome Serve(BackendContext& context) {
  context.maintenance =
      std::make_unique<CatalogMaintenance>(*context.repository, *context.coordinator);
  context.access = std::make_unique<AccessOrchestrator>(context.catalog, *context.repository,
                                                        *context.coordinator);
  context.mutations = std::make_unique<MutationOrchestrator>(context.catalog, *context.repository,
                                                             *context.coordinator);
  // Lifecycle thread assembles resources. Access preparation/evaluation runs on Access workers;
  // editing control and mutation protection stay on request coordination.
  context.request_coordinator = std::make_unique<RequestCoordinator>(
      context.runtime_limits->requests, context.runtime_limits->access_workers,
      [&context](std::exception_ptr error) { context.fatal_errors.report(error); },
      [&](OperationRequest request, Session session) -> RequestCoordinator::OperationAction {
        if (auto* check = std::get_if<CheckConsistencyRequest>(&request))
          return [&context, scope = check->scope] { return context.maintenance->check(scope); };
        if (auto* refresh = std::get_if<RefreshRequest>(&request))
          return [&context, scope = refresh->scope] { return context.maintenance->refresh(scope); };
        if (std::holds_alternative<MutationRequest>(request) ||
            std::holds_alternative<AcquireEditRequest>(request) ||
            std::holds_alternative<ReleaseEditRequest>(request))
          return context.mutations->prepare(std::move(request), std::move(session));
        return context.access->prepare(std::move(request));
      });
  context.runtime = std::make_unique<IpcServer>(*context.request_coordinator, context.fatal_errors,
                                                *context.ownership, "", *context.runtime_limits);
  auto broadcast = [&context](const PushNotification& event) {
    if (context.runtime) context.runtime->publish(event);
  };
  context.mutations->set_lock_observer([broadcast, &context](const core::Id& id) {
    try {
      broadcast(PushNotification{EditLockChangeNotice{id}});
    } catch (...) {
      context.fatal_errors.report(std::current_exception());
    }
  });
  context.maintenance->set_refresh_observer([broadcast, &context](const RefreshScope& scope) {
    try {
      broadcast(PushNotification{CatalogChangeNotice{scope, true}});
    } catch (...) {
      context.fatal_errors.report(std::current_exception());
    }
  });
  context.coordinator->set_change_observer(
      [broadcast, &context](const storage::CatalogCoordinator::Change& change) {
        try {
          if (const auto* record =
                  std::get_if<storage::CatalogCoordinator::RecordChange>(&change.value)) {
            const DirectoryRef target = record->directory_id ? DirectoryRef{*record->directory_id}
                                                             : DirectoryRef{record->type};
            const bool moved = record->previous_location && *record->previous_location != target;
            const auto& id = record->id;
            broadcast(
                PushNotification{RecordChangeNotice{id, true, change.content || change.uncertain}});
            broadcast(PushNotification{
                CatalogChangeNotice{DirectoryScanScope{target, RefreshTraversal::Immediate}}});
            if (moved)
              broadcast(PushNotification{CatalogChangeNotice{
                  DirectoryScanScope{*record->previous_location, RefreshTraversal::Immediate}}});
          } else if (const auto* directory =
                         std::get_if<storage::CatalogCoordinator::DirectoryChange>(&change.value)) {
            const auto type = directory->kind == core::DirectoryKind::Memo ? core::RecordType::Memo
                                                                           : core::RecordType::Todo;
            broadcast(PushNotification{
                CatalogChangeNotice{DirectoryScanScope{type, RefreshTraversal::Immediate}}});
          } else if (const auto* status = std::get_if<storage::CatalogStatus>(&change.value)) {
            const auto availability =
                *status == storage::CatalogStatus::Ready          ? SharedAvailability::Ready
                : *status == storage::CatalogStatus::Inconsistent ? SharedAvailability::Inconsistent
                                                                  : SharedAvailability::Unavailable;
            broadcast(PushNotification{AvailabilityChangeNotice{availability}});
            // Uncertain authoritative effects may not have reached Catalog publication.
            if (change.uncertain)
              broadcast(PushNotification{CatalogChangeNotice{RepositoryScope{}, true}});
          } else
            broadcast(PushNotification{CatalogChangeNotice{RepositoryScope{}, true}});
        } catch (...) {
          context.fatal_errors.report(std::current_exception());
        }
      },
      [&context] { context.fatal_errors.report(std::current_exception()); });
  auto result = context.runtime->run();
  if (auto* error = std::get_if<IpcError>(&result)) {
    return record_backend_failure(context, {Phase::Serve, *error});
  }
  return BackendActionOutcome::Success;
}

namespace {
// Lifecycle owns execution shutdown, including partial IPC initialization failures.
void drain_backend_requests(BackendContext& context) {
  if (context.runtime) context.runtime->stop_admission();
  if (context.request_coordinator) context.request_coordinator->drain();
  if (context.runtime) context.runtime->wait_for_responses();
}
} // namespace

BackendActionOutcome Stopping(BackendContext& context) {
  bool failed = false;
  if (context.runtime)
    context.runtime->publish(
        PushNotification{AvailabilityChangeNotice{SharedAvailability::Stopping}});
  drain_backend_requests(context);
  if (context.runtime) {
    for (auto& error : context.fatal_errors.take_failures()) {
      std::visit(
          [&](auto& cause) {
            record_backend_failure(context, {Phase::Stopping, std::move(cause)});
          },
          error);
      failed = true;
    }
  }
  if (context.ownership) {
    context.ownership->stop_listening();
  }
  return failed ? BackendActionOutcome::Failure : BackendActionOutcome::Success;
}

BackendActionOutcome Report(BackendContext& context) {
  // Failures can bypass Stopping. Settle accepted work while preserving streams.
  if (context.runtime || context.request_coordinator) {
    try {
      drain_backend_requests(context);
    } catch (...) {
      // Operation exceptions are already contained by the executor. Retain an
      // unexpected preparation failure without skipping the reporting action.
      // Do not claim execution stopped: Finish must retry before finalization.
      record_backend_failure(context, {Phase::Report, std::current_exception()});
    }
    for (auto& error : context.fatal_errors.take_failures()) {
      std::visit(
          [&](auto& cause) { record_backend_failure(context, {Phase::Report, std::move(cause)}); },
          error);
    }
  }
  if (context.runtime && !context.diagnostics.empty()) {
    // Report owns fatal shared-service reporting. Never broadcast raw diagnostics,
    // Entry paths, parser text or another Frontend's operation failure.
    context.runtime->publish(PushNotification{AvailabilityChangeNotice{
        SharedAvailability::Fatal,
        ApplicationError{
            ErrorCategory::ApplicationUnavailable,
            ErrorReason::BackendUnavailable,
            "The Backend encountered an unrecoverable service failure and is stopping.",
            {ErrorAction::Reconnect, ErrorAction::Dismiss}}}});
  }

  // Report is blocking: Finish may close sessions only after delivery completes,
  // a recipient fails, or the configured delivery timeout expires.
  if (context.runtime) context.runtime->wait_for_push_delivery();

  // Preserve startup rejection where IPC exists. Final reporting to active
  // Frontends and pre-IPC bootstrap delivery await the designer's protocol.
  if (!context.initialized && context.ownership) {
    const auto failure = context.diagnostics.empty()
                             ? "Backend startup failed"
                             : diagnostic_message(context.diagnostics.front());
    RequestCoordinator reporting_coordinator(
        context.runtime_limits->requests, context.runtime_limits->access_workers,
        [&context](std::exception_ptr error) { context.fatal_errors.report(error); });
    IpcServer reporting(reporting_coordinator, context.fatal_errors, *context.ownership, failure,
                        *context.runtime_limits);
    auto result = reporting.run();
    if (auto* error = std::get_if<IpcError>(&result)) {
      record_backend_failure(context, {Phase::Report, *error});
    }
  }
  return BackendActionOutcome::Success;
}

BackendActionOutcome Finish(BackendContext& context) {
  bool failed = false;
  // Also drain when an exception bypassed Stopping. No operation may retain
  // Storage references when explicit, fallible Catalog finalization starts.
  drain_backend_requests(context);
  if (context.runtime) {
    context.runtime->close_connections();
    for (auto& error : context.fatal_errors.take_failures()) {
      std::visit(
          [&](auto& cause) { record_backend_failure(context, {Phase::Finish, std::move(cause)}); },
          error);
      failed = true;
    }
    context.runtime.reset();
  }
  // Also stop admission when an error bypassed Stopping. Keep Repo locks until
  // explicit session finalization has been attempted and resources released.
  if (context.ownership) {
    context.ownership->stop_listening();
  }
  auto cleanup_error = [&](BackendDiagnostic diagnostic) {
    record_backend_failure(context, std::move(diagnostic));
    // Already logged at the retention boundary; do not reenter Report or send
    // a second IPC report. Logging failure cannot prevent resource release.
  };
  try {
    // Failed startup must retain its Marker, not certify an incomplete Index.
    if (context.initialized) {
      auto result = context.coordinator->finish_session();
      if (auto* error = std::get_if<storage::CoordinatorError>(&result)) {
        failed = true;
        cleanup_error({Phase::Finish, *error});
      }
    }
  } catch (...) {
    failed = true;
    cleanup_error({Phase::Finish, std::current_exception()});
  }
  context.maintenance.reset();
  context.coordinator.reset();
  context.repository.reset();
  context.ownership.reset();
  return failed ? BackendActionOutcome::Failure : BackendActionOutcome::Success;
}

BackendStateMachine::Handlers register_backend_states(BackendContext& context) {
  return {{
      [&] { return Bootstrap(context); },
      [&] { return Serve(context); },
      [&] { return Stopping(context); },
      [&] { return Report(context); },
      [&] { return Finish(context); },
  }};
}

void retain_backend_notification_failures(BackendContext& context,
                                          const BackendStateMachine& machine) {
  // A diagnostic callback failure cannot prevent Finish. These exceptional
  // notification failures are retained locally after lifecycle completion.
  for (const auto& failure : machine.failed_notifications()) {
    record_backend_failure(context, {failure.phase, failure.original, failure.original_time});
    record_backend_failure(context,
                           {failure.phase, failure.notification_error, failure.notification_time});
  }
  // PushNotification failures are collected late; restore occurrence order so an
  // earlier service failure remains primary even if cleanup also failed.
  std::stable_sort(
      context.diagnostics.begin(), context.diagnostics.end(),
      [](const auto& left, const auto& right) { return left.occurred_at < right.occurred_at; });
}

std::string diagnostic_message(const BackendDiagnostic& diagnostic) {
  return std::visit(
      [&](const auto& error) -> std::string {
        using Error = std::decay_t<decltype(error)>;
        if constexpr (std::is_same_v<Error, std::exception_ptr>) {
          try {
            if (error) std::rethrow_exception(error);
          } catch (const std::exception& exception) {
            return std::string("Backend lifecycle exception: ") + exception.what();
          } catch (...) {
            return "Unknown Backend lifecycle exception";
          }
          return "Empty Backend exception diagnostic";
        } else {
          std::string message = error.message;
          if constexpr (std::is_same_v<Error, storage::CoordinatorError>) {
            if (diagnostic.phase == Phase::Bootstrap) {
              message = "Backend initialization failed: " + message;
            }
          } else if (error.system_error) {
            message += ": " + error.system_error.message();
          }
          return message;
        }
      },
      diagnostic.error);
}

BackendActionOutcome record_backend_failure(BackendContext& context, BackendDiagnostic diagnostic) {
  // Never log arbitrary error messages, exception text, paths or payloads:
  // parser/codec errors can include sensitive user-controlled Entry content.
  // Retain the full typed diagnostic separately for the existing error protocol.
  try {
    static constexpr const char* phases[] = {"Bootstrap", "Serve", "Stopping", "Report", "Finish"};
    const auto* phase = phases[static_cast<std::size_t>(diagnostic.phase)];
    const auto* priority = context.diagnostics.empty() ? "primary" : "secondary";
    std::visit(
        [&](const auto& error) {
          using Error = std::decay_t<decltype(error)>;
          if constexpr (std::is_same_v<Error, std::exception_ptr>) {
            SPDLOG_CRITICAL("Backend {} exception during {} (details retained internally)",
                            priority, phase);
          } else if constexpr (std::is_same_v<Error, storage::CoordinatorError>) {
            SPDLOG_ERROR("Backend {} failure during {}: CoordinatorError code={} storage_code={} "
                         "file_code={} read_code={} codec_code={} system_error={}",
                         priority, phase, static_cast<int>(error.code),
                         error.storage_error ? static_cast<int>(error.storage_error->code) : -1,
                         error.file_error ? static_cast<int>(error.file_error->code) : -1,
                         error.binary_error ? static_cast<int>(error.binary_error->code) : -1,
                         error.codec_error ? static_cast<int>(error.codec_error->code) : -1,
                         error.file_error     ? error.file_error->system_error.value()
                         : error.binary_error ? error.binary_error->system_error.value()
                                              : 0);
          } else {
            const auto* type = std::is_same_v<Error, config::ConfigurationError>
                                   ? "ConfigurationError"
                               : std::is_same_v<Error, ProcessError> ? "ProcessError"
                                                                     : "IpcError";
            if constexpr (std::is_same_v<Error, ProcessError>) {
              if (error.code == ProcessErrorCode::AlreadyOwned) {
                SPDLOG_WARN("Backend ownership already held by another process");
                return;
              }
            }
            SPDLOG_ERROR("Backend {} failure during {}: {} code={} system_error={}", priority,
                         phase, type, static_cast<int>(error.code), error.system_error.value());
          }
        },
        diagnostic.error);
  } catch (...) {
    // Logging is independent of diagnostic retention and lifecycle transitions.
  }
  context.diagnostics.push_back(std::move(diagnostic));
  if (context.exit_code == 0) {
    context.exit_code = 1;
  }
  return BackendActionOutcome::Failure;
}

} // namespace folio::application::internal
