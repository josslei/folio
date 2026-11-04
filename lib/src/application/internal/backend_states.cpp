#include "application/internal/backend_states.hpp"
#include "application/internal/application_codec.hpp"
#include "application/internal/message_transport.hpp"
#include <folio/application/ipc.hpp>
#include <folio/config/configuration.hpp>
#include <folio/logging/logging.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace folio::application::internal {
using Phase = BackendStateMachine::State;
namespace {
core::Result<std::monostate, IpcError> serve(
    RepositoryOwnership& ownership, const std::string& startup_failure,
    BackendContext* context = nullptr);
}  // namespace

BackendActionOutcome Bootstrap(BackendContext& context) {
  if (context.startup_paths) {
    auto validated = config::validate_effective_paths(*context.startup_paths);
    if (auto* error = std::get_if<config::ConfigurationError>(&validated)) {
      return record_backend_failure(context, {Phase::Bootstrap, *error});
    }
    context.paths = std::get<config::StoragePaths>(std::move(validated));
  } else {
    auto resolved = config::load_configuration();
    if (auto* error = std::get_if<config::ConfigurationError>(&resolved)) {
      return record_backend_failure(context, {Phase::Bootstrap, *error});
    }
    auto effective = std::get<config::EffectiveConfig>(std::move(resolved));
    logging::set_level(effective.log_level);
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
  context.repository =
      std::make_unique<storage::NormalRepository>(context.paths->normal_root);
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
  auto result = serve(*context.ownership, {}, &context);
  if (auto* error = std::get_if<IpcError>(&result)) {
    return record_backend_failure(context, {Phase::Serve, *error});
  }
  return BackendActionOutcome::Success;
}

BackendActionOutcome Stopping(BackendContext& context) {
  if (context.ownership) {
    context.ownership->stop_listening();
  }
  return BackendActionOutcome::Success;
}

BackendActionOutcome Report(BackendContext& context) {
  // Preserve startup rejection where IPC exists. Final reporting to active
  // Frontends and pre-IPC bootstrap delivery await the designer's protocol.
  if (!context.initialized && context.ownership) {
    const auto failure =
        context.diagnostics.empty() ? "Backend startup failed"
                                    : diagnostic_message(context.diagnostics.front());
    auto result = serve(*context.ownership, failure);
    if (auto* error = std::get_if<IpcError>(&result)) {
      record_backend_failure(context, {Phase::Report, *error});
    }
  }
  return BackendActionOutcome::Success;
}

BackendActionOutcome Finish(BackendContext& context) {
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
  bool failed = false;
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
  context.coordinator.reset();
  context.repository.reset();
  context.ownership.reset();
  return failed ? BackendActionOutcome::Failure : BackendActionOutcome::Success;
}

namespace {
using Clock = std::chrono::steady_clock;

struct Peer {
  IpcStream stream;
  MessageTransport messages;
  Clock::time_point request_deadline{};
  bool active = false, registering = false, close_after_output = false;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
};

// This loop owns lifecycle and synchronous read execution. No handler outlives
// its request. Future queued/active mutations must drain before finish_session().
core::Result<std::monostate, IpcError> serve(
    RepositoryOwnership& ownership, const std::string& startup_failure,
    BackendContext* context) {
  std::vector<Peer> peers;
  bool served = false;
  const auto initial_deadline =
      Clock::now() +
      (startup_failure.empty() ? std::chrono::seconds(10) : std::chrono::seconds(2));
  for (;;) {
    // Bound admission and per-peer work so malformed clients cannot starve others.
    if (peers.size() < 64) {
      auto accepted = ownership.accept();
      if (auto* problem = std::get_if<IpcError>(&accepted))
        return *problem;
      if (auto& stream = std::get<std::optional<IpcStream>>(accepted); stream)
        peers.push_back(Peer{std::move(*stream)});
    }
    for (auto it = peers.begin(); it != peers.end();) {
      auto& peer = *it;
      bool remove = false;
      auto queue = [&](MessageKind kind, std::vector<std::byte> payload) {
        auto result = peer.messages.queue({kind, std::move(payload)});
        if (std::holds_alternative<IpcError>(result)) remove = true;
        peer.deadline = Clock::now() + std::chrono::seconds(3);
      };
      auto reject = [&](const std::string& reason) {
        queue(MessageKind::Reject, payload_bytes(reason));
        peer.close_after_output = true;
      };
      if (peer.messages.sending()) {
        auto written = peer.messages.flush(peer.stream);
        if (std::holds_alternative<IpcError>(written)) {
          remove = true;
        } else {
          peer.deadline = peer.messages.last_activity() + std::chrono::seconds(3);
          if (!peer.messages.sending()) {
            if (peer.close_after_output) remove = true;
            if (peer.registering) {
              peer.active = true;
              peer.registering = false;
              served = true;
            }
          }
        }
      } else {
        if (!peer.messages.receiving())
          peer.request_deadline = Clock::now() + std::chrono::seconds(30);
        auto received = peer.messages.receive(peer.stream);
        peer.deadline = peer.messages.last_activity() + std::chrono::seconds(3);
        if (auto* error = std::get_if<IpcError>(&received)) {
          if (error->code == IpcErrorCode::Protocol || error->code == IpcErrorCode::Incompatible)
            reject(error->message);
          else
            remove = true;
        } else if (auto& message = std::get<std::optional<Message>>(received); message) {
          if (!startup_failure.empty()) {
            reject(startup_failure);
          } else if (!peer.active && message->kind == MessageKind::Hello && message->payload.empty()) {
            queue(MessageKind::Ready, {});
            peer.registering = true;
          } else if (peer.active && message->kind == MessageKind::Ping && message->payload.empty()) {
            queue(MessageKind::Pong, {});
          } else if (peer.active && context && message->kind == MessageKind::Request) {
            auto request = decode_access_request(payload_text(message->payload));
            AccessResponse result = std::holds_alternative<IpcError>(request)
                ? AccessResponse{ApplicationError{ErrorCategory::InvalidRequest,
                    ErrorReason::InvalidQuery, "The read request is malformed or contains invalid values.",
                    {ErrorAction::Dismiss}}}
                : handle_access(std::get<AccessRequest>(request), context->catalog,
                                *context->repository, *context->coordinator);
            auto encoded = encode_access_response(result);
            if (std::holds_alternative<IpcError>(encoded)) {
              encoded = encode_access_response(ApplicationError{
                  ErrorCategory::ApplicationUnavailable, ErrorReason::ResourceLimit,
                  "The read response could not be represented within the resource limit.",
                  {ErrorAction::Dismiss}});
            }
            if (auto* bytes = std::get_if<std::string>(&encoded))
              queue(MessageKind::Response, payload_bytes(*bytes));
            else
              reject("Unable to encode Application response");
          } else {
            reject("Invalid lifecycle message");
          }
        }
      }
      if (peer.messages.receiving() && Clock::now() >= peer.request_deadline)
        remove = true;
      if ((!peer.active || peer.messages.receiving() || peer.messages.sending()) &&
          Clock::now() >= peer.deadline)
        remove = true;
      if (remove)
        it = peers.erase(it);
      else
        ++it;
    }
    const bool active =
        std::any_of(peers.begin(), peers.end(), [](const Peer& p) { return p.active; });
    // Once served, zero registered clients ends admission immediately. Pending
    // handshakes can retry a new Backend; none receives Ready after this decision.
    if ((served && !active) || (!served && Clock::now() >= initial_deadline))
      return std::monostate{};
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

}  // namespace

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
    record_backend_failure(
        context, {failure.phase, failure.original, failure.original_time});
    record_backend_failure(
        context, {failure.phase, failure.notification_error, failure.notification_time});
  }
  // Notification failures are collected late; restore occurrence order so an
  // earlier service failure remains primary even if cleanup also failed.
  std::stable_sort(context.diagnostics.begin(), context.diagnostics.end(),
                   [](const auto& left, const auto& right) {
                     return left.occurred_at < right.occurred_at;
                   });
}

std::string diagnostic_message(const BackendDiagnostic& diagnostic) {
  return std::visit([&](const auto& error) -> std::string {
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
  }, diagnostic.error);
}

BackendActionOutcome record_backend_failure(BackendContext& context,
                                            BackendDiagnostic diagnostic) {
  // Never log arbitrary error messages, exception text, paths or payloads:
  // parser/codec errors can include sensitive user-controlled Entry content.
  // Retain the full typed diagnostic separately for the existing error protocol.
  try {
    static constexpr const char* phases[] = {"Bootstrap", "Serve", "Stopping", "Report", "Finish"};
    const auto* phase = phases[static_cast<std::size_t>(diagnostic.phase)];
    const auto* priority = context.diagnostics.empty() ? "primary" : "secondary";
    std::visit([&](const auto& error) {
      using Error = std::decay_t<decltype(error)>;
      if constexpr (std::is_same_v<Error, std::exception_ptr>) {
        SPDLOG_CRITICAL("Backend {} exception during {} (details retained internally)", priority, phase);
      } else if constexpr (std::is_same_v<Error, storage::CoordinatorError>) {
        SPDLOG_ERROR("Backend {} failure during {}: CoordinatorError code={} storage_code={} file_code={} read_code={} codec_code={} system_error={}",
                     priority, phase, static_cast<int>(error.code),
                     error.storage_error ? static_cast<int>(error.storage_error->code) : -1,
                     error.file_error ? static_cast<int>(error.file_error->code) : -1,
                     error.binary_error ? static_cast<int>(error.binary_error->code) : -1,
                     error.codec_error ? static_cast<int>(error.codec_error->code) : -1,
                     error.file_error ? error.file_error->system_error.value()
                         : error.binary_error ? error.binary_error->system_error.value() : 0);
      } else {
        const auto* type = std::is_same_v<Error, config::ConfigurationError> ? "ConfigurationError"
                         : std::is_same_v<Error, ProcessError> ? "ProcessError" : "IpcError";
        if constexpr (std::is_same_v<Error, ProcessError>) {
          if (error.code == ProcessErrorCode::AlreadyOwned) {
            SPDLOG_WARN("Backend ownership already held by another process");
            return;
          }
        }
        SPDLOG_ERROR("Backend {} failure during {}: {} code={} system_error={}",
                     priority, phase, type, static_cast<int>(error.code), error.system_error.value());
      }
    }, diagnostic.error);
  } catch (...) {
    // Logging is independent of diagnostic retention and lifecycle transitions.
  }
  context.diagnostics.push_back(std::move(diagnostic));
  if (context.exit_code == 0) {
    context.exit_code = 1;
  }
  return BackendActionOutcome::Failure;
}

}  // namespace folio::application::internal
