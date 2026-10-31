#include "application/internal/backend_states.hpp"
#include "application/internal/configuration.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

namespace folio::application::internal {
using Phase = BackendStateMachine::State;
namespace {
core::Result<std::monostate, IpcError> serve(
    RepositoryOwnership& ownership, const std::string& startup_failure);
}  // namespace

BackendActionOutcome Bootstrap(BackendContext& context) {
  auto resolved = load_configuration();
  if (auto* error = std::get_if<ProcessError>(&resolved)) {
    return record_backend_failure(context, {Phase::Bootstrap, *error});
  }
  context.paths.emplace(std::get<storage::StoragePaths>(std::move(resolved)));
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
  context.initialized = true;
  return BackendActionOutcome::Success;
}

BackendActionOutcome Serve(BackendContext& context) {
  auto result = serve(*context.ownership, {});
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
  for (const auto& diagnostic : context.diagnostics) {
    std::cerr << diagnostic_message(diagnostic) << '\n';
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
    // Retain cleanup errors, but do not reenter Report or send another IPC report.
    // Local stderr is temporary until the logging system is designed. A failed
    // local output must not prevent release of the remaining resources.
    try {
      std::cerr << diagnostic_message(context.diagnostics.back()) << '\n';
    } catch (...) {
      record_backend_failure(context, {Phase::Finish, std::current_exception()});
    }
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
  std::string input, output;
  bool active = false, registering = false, close_after_output = false;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
};

// Lifecycle has one owner: this loop. There are no application operations yet.
// Future queued/active work must be drained here before finish_session().
core::Result<std::monostate, IpcError> serve(
    RepositoryOwnership& ownership, const std::string& startup_failure) {
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
        peers.push_back(Peer{std::move(*stream), {}, {}});
    }
    for (auto it = peers.begin(); it != peers.end();) {
      auto& peer = *it;
      bool remove = false;
      if (!peer.output.empty()) {
        auto written = peer.stream.write(
            std::as_bytes(std::span(peer.output.data(), peer.output.size())));
        if (std::holds_alternative<IpcError>(written))
          remove = true;
        else {
          peer.output.erase(0, std::get<std::size_t>(written));
          if (peer.output.empty()) {
            if (peer.close_after_output)
              remove = true;
            if (peer.registering) {
              peer.active = true;
              peer.registering = false;
              served = true;
            }
          }
        }
      } else {
        std::array<std::byte, 4096> bytes{};
        auto received = peer.stream.read(bytes);
        if (std::holds_alternative<IpcError>(received))
          remove = true;
        else {
          auto count = std::get<std::size_t>(received);
          if (count && peer.input.empty())
            peer.deadline = Clock::now() + std::chrono::seconds(3);
          peer.input += payload_text(std::span(bytes).first(count));
          auto decoded = decode_message(peer.input);
          auto reject = [&](std::string message) {
            peer.output = encode_message({MessageKind::Reject, payload_bytes(message)});
            peer.close_after_output = true;
            peer.deadline = Clock::now() + std::chrono::seconds(3);
          };
          if (auto* problem = std::get_if<IpcError>(&decoded))
            reject(problem->message);
          else if (auto& message = std::get<std::optional<Message>>(decoded); message) {
            if (!startup_failure.empty())
              reject(startup_failure);
            else if (!peer.active && message->kind == MessageKind::Hello &&
                     message->payload.empty()) {
              peer.output = encode_message({MessageKind::Ready, {}});
              peer.registering = true;
            } else if (peer.active && message->kind == MessageKind::Ping &&
                       message->payload.empty()) {
              peer.output = encode_message({MessageKind::Pong, {}});
            } else
              reject("Invalid lifecycle message");
          }
        }
      }
      if ((!peer.active || !peer.input.empty() || !peer.output.empty()) &&
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
  context.diagnostics.push_back(std::move(diagnostic));
  if (context.exit_code == 0) {
    context.exit_code = 1;
  }
  return BackendActionOutcome::Failure;
}

}  // namespace folio::application::internal
