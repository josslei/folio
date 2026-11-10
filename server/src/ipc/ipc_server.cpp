#include "ipc/ipc_server.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <folio/logging/logging.hpp>

#include <algorithm>

namespace folio::application::internal {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto forever = Clock::time_point::max();

ApplicationError capacity_error() {
  return {ErrorCategory::ApplicationUnavailable,
          ErrorReason::ResourceLimit,
          "The Backend cannot admit another request right now.",
          {ErrorAction::Retry, ErrorAction::Dismiss}};
}
} // namespace

IpcServer::IpcServer(RepositoryOwnership& ownership, Handler handler, std::string startup_failure,
                     RuntimeLimits limits, Preparer prepare)
    : ownership_(ownership), handler_(std::move(handler)), prepare_(std::move(prepare)),
      startup_failure_(std::move(startup_failure)), limits_(limits),
      request_coordinator_(limits.requests, limits.access_workers) {}

IpcServer::~IpcServer() {
  drain_requests();
  close_connections();
}

void IpcServer::notify() {
  changed_.request_stop();
}

void IpcServer::failure(Failure error) {
  std::lock_guard lock(connection_state_mutex_);
  failures_.push_back(std::move(error));
  notify();
}

std::shared_ptr<IpcServer::BufferReservation> IpcServer::reserve_request() {
  auto count = reservations_.load();
  while (count < limits_.buffered_requests) {
    if (reservations_.compare_exchange_weak(count, count + 1)) {
      try {
        return std::make_shared<BufferReservation>(reservations_);
      } catch (...) {
        --reservations_;
        throw;
      }
    }
  }
  return {};
}

bool IpcServer::send_rejection(IpcStream& stream, ApplicationError error, std::stop_token stop) {
  auto encoded = codec::encode(error);
  if (auto* bytes = std::get_if<std::string>(&encoded)) {
    return std::holds_alternative<std::monostate>(send_message(
        stream, {MessageKind::Rejected, payload_bytes(*bytes)}, Clock::now() + 3s, stop));
  }
  return false;
}

void IpcServer::connection(Connection& connection, IpcStream& stream, std::stop_token stop) {
  // Connection worker: handshake and session registration begin here. This worker
  // owns the stream; Application handlers execute on the request coordinator's executor thread.
  std::stop_source reception;
  std::stop_callback closing(stop, [&] { reception.request_stop(); });
  std::stop_callback admission(admission_closed_.get_token(), [&] { reception.request_stop(); });
  const auto receive_stop = reception.get_token();
  const auto session = std::make_shared<ConnectionSession>();
  bool registered = false;
  // Registration must be undone on every ordinary return and exception.
  struct Registration {
    IpcServer& runtime;
    bool& registered;
    ~Registration() {
      std::lock_guard lock(runtime.connection_state_mutex_);
      if (registered) --runtime.active_;
      runtime.notify();
    }
  } registration{*this, registered};

  auto send = [&](MessageKind kind, std::vector<std::byte> payload = {}) {
    return std::holds_alternative<std::monostate>(
        send_message(stream, {kind, std::move(payload)}, Clock::now() + 3s, stop));
  };
  auto reject_protocol = [&](const std::string& text) {
    send(MessageKind::Reject, payload_bytes(text));
  };
  MessageTransport handshake([](MessageKind kind, std::size_t size) {
    // Lifecycle messages have no body. Refuse oversized handshakes before they
    // can bypass Application request reservations.
    return kind == MessageKind::Hello && size == 4;
  });
  auto hello = receive_message(stream, handshake, Clock::now() + 3s, stop);
  if (auto* error = std::get_if<IpcError>(&hello)) {
    if (error->code == IpcErrorCode::Protocol || error->code == IpcErrorCode::Incompatible)
      reject_protocol(error->message);
    return;
  }
  if (!startup_failure_.empty()) {
    reject_protocol(startup_failure_);
    return;
  }
  const auto& message = std::get<Message>(hello);
  if (message.kind != MessageKind::Hello || !message.payload.empty()) {
    reject_protocol("Invalid readiness handshake");
    return;
  }
  {
    std::lock_guard lock(connection_state_mutex_);
    if (stopping_) return;
    ++active_;
    registered = true;
  }
  if (!send(MessageKind::Ready)) return;
  {
    std::lock_guard lock(connection_state_mutex_);
    served_ = true;
    notify();
  }

  // Connection worker: each iteration receives at most one outstanding request.
  while (!stop.stop_requested()) {
    {
      std::lock_guard lock(connection_state_mutex_);
      if (stopping_) return;
    }
    std::shared_ptr<BufferReservation> reservation;
    bool limited = false;
    MessageTransport input([&](MessageKind kind, std::size_t size) {
      if (kind != MessageKind::Request) return kind == MessageKind::Ping && size == 4;
      reservation = reserve_request();
      limited = !reservation;
      return !limited;
    });
    // An idle established connection has no artificial expiry. Once bytes
    // arrive, bounded complete-request reception begins.
    auto ready = stream.wait(false, forever, receive_stop);
    if (!std::holds_alternative<bool>(ready) || !std::get<bool>(ready)) return;
    auto received = receive_message(stream, input, Clock::now() + 30s, receive_stop);
    if (auto* error = std::get_if<IpcError>(&received)) {
      if (limited)
        send_rejection(stream, capacity_error(), stop);
      else if (error->code == IpcErrorCode::Protocol || error->code == IpcErrorCode::Incompatible)
        reject_protocol(error->message);
      return; // An early capacity rejection leaves an unread body: close it.
    }
    auto request_message = std::get<Message>(std::move(received));
    if (request_message.kind == MessageKind::Ping && request_message.payload.empty()) {
      if (!send(MessageKind::Pong)) return;
      continue;
    }
    if (request_message.kind != MessageKind::Request) {
      reject_protocol("Expected one Application request");
      return;
    }
    // Connection worker: complete-message decoding/validation precedes admission.
    auto request = codec::decode<OperationRequest>(payload_text(request_message.payload));
    // Malformed complete Application requests are rejected before acceptance;
    // their connection is still usable, as with prior Application errors.
    if (std::holds_alternative<IpcError>(request)) {
      if (!send_rejection(stream,
                          {ErrorCategory::InvalidRequest,
                           ErrorReason::InvalidQuery,
                           "The Application request is malformed or contains invalid values.",
                           {ErrorAction::Dismiss}},
                          stop))
        return;
      continue;
    }
    // Free the wire buffer before keeping the decoded request in the queue.
    request_message.payload.clear();
    request_message.payload.shrink_to_fit();
    // Connection worker: admission atomically checks server stopping state and
    // enqueues into the shared coordinator. The lambda is stored, not run inline.
    std::shared_ptr<RequestCoordinator::Ticket> ticket;
    {
      std::lock_guard lock(connection_state_mutex_);
      if (!stopping_) {
        auto value = std::get<OperationRequest>(std::move(request));
        const bool mutation = std::holds_alternative<MutationRequest>(value);
        const auto plan = prepare_ ? RequestCoordinator::plan(value) : RequestCoordinator::Plan{};
        ticket = request_coordinator_.admit(
            plan,
            [this, value = std::move(value), session, reservation,
             mutation]() mutable -> RequestCoordinator::Action {
              // Access worker: prepare business state. Mutation/control preparation
              // instead runs briefly on request coordination.
              try {
                auto action = prepare_ ? prepare_(std::move(value), session)
                                       : PreparedAction{[this, value = std::move(value), session] {
                                           return handler_(value, session);
                                         }};
                return [this, action = std::move(action), reservation, mutation]() {
                  // Execution worker (or brief editing control): produce and encode
                  // the final result without server/queue locks held.
                  try {
                    return encode_response(action(), mutation);
                  } catch (...) {
                    failure(std::current_exception());
                    throw;
                  }
                };
              } catch (...) {
                failure(std::current_exception());
                throw;
              }
            });
        if (ticket) connection.busy = true;
      }
    }
    if (!ticket) {
      if (!send_rejection(stream, capacity_error(), stop)) return;
      continue;
    }
    auto delivery_failure = [&](const char* point) {
      try {
        std::lock_guard lock(ticket->mutex);
        SPDLOG_WARN(
            "Accepted Application request lost {} (execution_stage={}); no cancellation or replay",
            point, static_cast<int>(ticket->stage));
      } catch (...) {
        // Logging must not change scheduling or the retained operation result.
      }
    };
    // Connection worker: Accepted acknowledges admission, not execution success.
    // Failure to deliver Accepted never retracts accepted work.
    if (!send(MessageKind::Accepted)) {
      delivery_failure("admission acknowledgment");
      return;
    }

    // Wait for execution OR peer data/EOF. The completion signal is not a
    // cancellation request: disconnecting drops delivery, never the queued job.
    std::stop_callback shutdown(stop, [&] { ticket->completed.request_stop(); });
    while (!ticket->completed.stop_requested()) {
      auto pending = stream.wait(false, forever, ticket->completed.get_token());
      if (std::holds_alternative<IpcError>(pending)) {
        delivery_failure("connection while awaiting execution");
        return;
      }
      if (std::get<bool>(pending)) {
        // A connection may not pipeline another request while one is pending.
        // connected() differentiates EOF without consuming a second request.
        if (stream.connected()) reject_protocol("Only one outstanding request is permitted");
        delivery_failure("connection while awaiting execution");
        return;
      }
    }
    if (stop.stop_requested()) return;
    std::optional<RequestCoordinator::Result> result;
    {
      std::lock_guard lock(ticket->mutex);
      result = std::move(ticket->result);
    }
    if (!result) return;
    if (std::holds_alternative<IpcError>(*result))
      return; // Execution failures were retained at their execution boundary.
    // Connection worker: deliver the executor's final response after Accepted.
    auto final = std::get<Message>(std::move(*result));
    if (!std::holds_alternative<std::monostate>(
            send_message(stream, final, Clock::now() + 30s, stop))) {
      delivery_failure("final response");
      return;
    }
    {
      std::lock_guard lock(connection_state_mutex_);
      connection.busy = false;
      workers_changed_.notify_all();
    }
  }
}

RequestCoordinator::Result IpcServer::encode_response(OperationResponse response, bool mutation) {
  auto encoded = codec::encode<OperationResponse>(response);
  if (std::holds_alternative<IpcError>(encoded)) {
    if (mutation) {
      try {
        SPDLOG_ERROR("Mutation final response exceeded protocol limits; confirmation lost");
      } catch (...) {
      }
      return std::get<IpcError>(encoded);
    }
    encoded = codec::encode<OperationResponse>(
        ApplicationError{ErrorCategory::ApplicationUnavailable,
                         ErrorReason::ResourceLimit,
                         "The Application response exceeds the resource limit.",
                         {ErrorAction::Dismiss}});
  }
  if (auto* error = std::get_if<IpcError>(&encoded)) {
    failure(*error);
    return *error;
  }
  return Message{MessageKind::Response, payload_bytes(std::get<std::string>(encoded))};
}

core::Result<std::monostate, IpcError> IpcServer::run(std::stop_token shutdown) {
  // Backend lifecycle/main thread: accept connections and start their workers.
  // The shared coordinator's executor was already started during construction.
  struct StopAdmission {
    IpcServer& server;
    ~StopAdmission() { server.stop_admission(); }
  } stop_admission_on_exit{*this};
  std::stop_callback interrupt(shutdown, [&] {
    std::lock_guard lock(connection_state_mutex_);
    notify();
  });
  const auto initial_deadline = Clock::now() + (startup_failure_.empty() ? 10s : 2s);
  for (;;) {
    std::vector<std::unique_ptr<Connection>> finished;
    std::stop_token changed;
    Clock::time_point deadline;
    {
      std::lock_guard lock(connection_state_mutex_);
      // Install a fresh signal before testing predicates under the same lock
      // used by registration/disconnection, avoiding lost wakeups.
      changed_ = std::stop_source{};
      changed = changed_.get_token();
      for (auto it = connections_.begin(); it != connections_.end();) {
        if ((*it)->done) {
          finished.push_back(std::move(*it));
          it = connections_.erase(it);
        } else
          ++it;
      }
      if (shutdown.stop_requested() || !failures_.empty() || (served_ && active_ == 0) ||
          (!served_ && active_ == 0 && Clock::now() >= initial_deadline)) {
        stopping_ = true;
        request_coordinator_.stop_admission();
        if (!failures_.empty()) {
          auto error = std::move(failures_.front());
          failures_.erase(failures_.begin());
          if (auto* ipc = std::get_if<IpcError>(&error)) return *ipc;
          std::rethrow_exception(std::get<std::exception_ptr>(error));
        }
        return std::monostate{};
      }
      deadline = served_ ? forever : initial_deadline;
    }
    finished.clear(); // Join completed workers without holding their mutex.
    if (connections_.size() < limits_.connections) {
      auto accepted = ownership_.accept();
      if (auto* error = std::get_if<IpcError>(&accepted)) return *error;
      if (auto& stream = std::get<std::optional<IpcStream>>(accepted); stream) {
        auto entry = std::make_unique<Connection>();
        auto* pointer = entry.get();
        entry->worker = std::jthread(
            [this, pointer, stream = std::move(*stream)](std::stop_token stop) mutable {
              try {
                connection(*pointer, stream, stop);
              } catch (...) {
                failure(std::current_exception());
              }
              // Preserve a usable stream through Report. Finish wakes this wait;
              // no new requests are received after admission closes.
              {
                std::unique_lock lock(connection_state_mutex_);
                pointer->busy = false;
                workers_changed_.notify_all();
                if (stopping_ && stream.connected()) {
                  std::condition_variable_any closed;
                  closed.wait(lock, stop, [] { return false; });
                }
              }
              std::lock_guard lock(connection_state_mutex_);
              pointer->done = true;
              notify();
            });
        connections_.push_back(std::move(entry));
        continue;
      }
    } else {
      // Capacity is bounded. Wait for a worker to leave, not a readable listener
      // with queued connections, which would otherwise cause a busy loop.
      std::mutex wait_mutex;
      std::condition_variable_any condition;
      std::unique_lock lock(wait_mutex);
      if (deadline == forever)
        condition.wait(lock, changed, [] { return false; });
      else
        condition.wait_until(lock, changed, deadline, [] { return false; });
      continue;
    }
    auto ready = ownership_.wait(deadline, changed);
    if (auto* error = std::get_if<IpcError>(&ready)) {
      if (error->code != IpcErrorCode::Timeout) return *error;
    }
  }
}

std::vector<IpcServer::Failure> IpcServer::take_failures() {
  std::lock_guard lock(connection_state_mutex_);
  std::vector<Failure> result;
  result.swap(failures_);
  return result;
}

void IpcServer::stop_admission() {
  {
    std::lock_guard lock(connection_state_mutex_);
    stopping_ = true;
    request_coordinator_.stop_admission();
    notify();
  }
  admission_closed_.request_stop();
  ownership_.stop_listening();
}

void IpcServer::drain_requests() {
  // Lifecycle thread (Stopping/Report): close admission, then join execution
  // outside the connection-state lock so actions can still publish failures.
  stop_admission();
  request_coordinator_.drain();
  // Let bounded final-response delivery settle before Finish closes streams.
  {
    std::unique_lock lock(connection_state_mutex_);
    workers_changed_.wait(lock, [&] {
      for (const auto& connection : connections_)
        if (connection->busy) return false;
      return true;
    });
  }
  // Reap disconnected workers without terminating usable connections.
  for (auto it = connections_.begin(); it != connections_.end();) {
    bool done;
    {
      std::lock_guard lock(connection_state_mutex_);
      done = (*it)->done;
    }
    if (done) {
      if ((*it)->worker.joinable()) (*it)->worker.join();
      it = connections_.erase(it);
    } else
      ++it;
  }
}

void IpcServer::close_connections() {
  // Lifecycle thread (Finish, after draining): wake/join connection workers
  // without holding the state lock their exit paths require.
  for (auto& connection : connections_)
    connection->worker.request_stop();
  for (auto& connection : connections_)
    if (connection->worker.joinable()) connection->worker.join();
  connections_.clear();
}

} // namespace folio::application::internal
