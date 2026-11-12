#include "ipc/ipc_server.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <folio/logging/logging.hpp>

#include <algorithm>

namespace folio::application::internal {
/// Transport state shared by the two session workers, never editing ownership.
/// Producers enqueue bounded encoded invalidations without performing I/O.
struct IpcServer::PushDelivery {
  /// Protects pairing, pending queue and queued wake-source replacement; the Push worker alone owns
  /// delivery/ACK I/O.
  std::mutex delivery_mutex;
  std::condition_variable_any changed;
  bool paired = false;
  std::deque<std::string> pending;
  std::stop_source queued;
  /// Independent cooperative session termination; safe to signal from either worker.
  std::stop_source ended;
  /// Terminates the logical session and wakes both workers; accepted work survives.
  void close() {
    ended.request_stop();
    changed.notify_all();
  }
};

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

IpcServer::IpcServer(RequestCoordinator& request_coordinator, FatalErrorReporter& fatal_errors,
                     RepositoryOwnership& ownership, std::string startup_failure,
                     RuntimeLimits limits)
    : fatal_error_reporter_(fatal_errors), ownership_(ownership),
      startup_failure_(std::move(startup_failure)), limits_(limits),
      request_coordinator_(request_coordinator) {}

IpcServer::~IpcServer() {
  stop_admission();
  close_connections();
}

void IpcServer::notify() {
  changed_.request_stop();
}

std::shared_ptr<IpcServer::BufferReservation> IpcServer::reserve_request() {
  auto count = reservations_->load();
  while (count < limits_.buffered_requests) {
    if (reservations_->compare_exchange_weak(count, count + 1)) {
      try {
        return std::make_shared<BufferReservation>(reservations_);
      } catch (...) {
        --*reservations_;
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

void IpcServer::serve_connection(Connection& connection, IpcStream& stream, std::stop_token stop) {
  // Connection worker: handshake and session registration begin here. This worker
  // owns the stream; Application handlers execute on the request coordinator's executor thread.
  auto io_stop = stop;
  auto send = [&](MessageKind kind, std::vector<std::byte> payload = {}) {
    return std::holds_alternative<std::monostate>(
        send_message(stream, {kind, std::move(payload)}, Clock::now() + 3s, io_stop));
  };
  auto reject_protocol = [&](const std::string& text) {
    send(MessageKind::Reject, payload_bytes(text));
  };
  MessageTransport handshake([](MessageKind kind, std::size_t size) {
    // Bound role-bearing Hello before allocating its payload.
    return kind == MessageKind::Hello && size <= 128;
  });
  auto hello = receive_message(stream, handshake, Clock::now() + 3s, stop);
  if (auto* error = std::get_if<IpcError>(&hello)) {
    if (error->code == IpcErrorCode::Protocol || error->code == IpcErrorCode::Incompatible)
      reject_protocol(error->message);
    return;
  }
  const auto& first = std::get<Message>(hello);
  auto decoded_hello = codec::decode<HelloPayload>(payload_text(first.payload));
  if (!std::holds_alternative<HelloPayload>(decoded_hello)) {
    reject_protocol("Invalid Hello payload");
    return;
  }
  const auto& role = std::get<HelloPayload>(decoded_hello);
  if (const auto* push = std::get_if<PushHello>(&role)) {
    std::shared_ptr<PushDelivery> delivery;
    {
      std::lock_guard lock(sessions_mutex_);
      auto found = pending_push_pairings_.find(push->pairing_token.to_string());
      if (found != pending_push_pairings_.end()) {
        delivery = found->second.lock();
        pending_push_pairings_.erase(found); // Pairing capability is single-use.
      }
    }
    if (!delivery || delivery->ended.stop_requested()) {
      reject_protocol("Invalid session pairing");
      return;
    }
    connection.delivery = delivery;
    serve_push_notification_connection(stream, std::move(delivery), stop);
    return;
  }
  if (!startup_failure_.empty()) {
    reject_protocol(startup_failure_);
    return;
  }
  const auto& message = std::get<Message>(hello);
  if (message.kind != MessageKind::Hello || !std::holds_alternative<RequestHello>(role)) {
    reject_protocol("Invalid readiness handshake");
    return;
  }
  serve_request_connection(connection, stream, stop);
}

void IpcServer::serve_request_connection(Connection& connection, IpcStream& stream,
                                         std::stop_token stop) {
  std::stop_source reception;
  std::stop_callback closing(stop, [&] { reception.request_stop(); });
  std::stop_callback admission(admission_closed_.get_token(), [&] { reception.request_stop(); });
  const auto receive_stop = reception.get_token();
  Session session;
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

  auto io_stop = stop;
  auto send = [&](MessageKind kind, std::vector<std::byte> payload = {}) {
    return std::holds_alternative<std::monostate>(
        send_message(stream, {kind, std::move(payload)}, Clock::now() + 3s, io_stop));
  };
  auto reject_protocol = [&](const std::string& text) {
    send(MessageKind::Reject, payload_bytes(text));
  };
  {
    std::lock_guard lock(connection_state_mutex_);
    if (stopping_) return;
    if (active_ >= limits_.connections) return;
    ++active_;
    registered = true;
  }
  connection.session = std::make_shared<ConnectionSession>();
  session = connection.session;
  auto delivery = std::make_shared<PushDelivery>();
  connection.delivery = delivery;
  io_stop = delivery->ended.get_token();
  const auto token = core::Id::generate().to_string();
  {
    std::lock_guard lock(sessions_mutex_);
    std::erase_if(pending_push_pairings_, [](const auto& item) { return item.second.expired(); });
    pending_push_pairings_[token] = delivery;
  }
  bool preserve_for_report = false;
  struct CloseSession {
    std::shared_ptr<PushDelivery> delivery;
    bool& preserve;
    ~CloseSession() {
      if (!preserve) delivery->close();
    }
  } close_session{delivery, preserve_for_report};
  std::stop_callback terminate(stop, [&] { delivery->close(); });
  std::stop_callback peer_failed(delivery->ended.get_token(), [&] { reception.request_stop(); });
  if (!send(MessageKind::Ready, payload_bytes(token))) return;
  {
    std::unique_lock lock(delivery->delivery_mutex);
    delivery->changed.wait_until(lock, delivery->ended.get_token(), Clock::now() + 3s, [&] {
      return delivery->paired || delivery->ended.stop_requested();
    });
    if (!delivery->paired || delivery->ended.stop_requested()) {
      delivery->close();
      return;
    }
  }
  {
    std::lock_guard lock(connection_state_mutex_);
    served_ = true;
    notify();
  }

  // Connection worker: each iteration receives at most one outstanding request.
  while (!stop.stop_requested() && !delivery->ended.stop_requested()) {
    {
      std::lock_guard lock(connection_state_mutex_);
      if (stopping_) {
        preserve_for_report = true;
        return;
      }
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
    if (!std::holds_alternative<bool>(ready) || !std::get<bool>(ready)) {
      preserve_for_report = admission_closed_.stop_requested() &&
                            !delivery->ended.stop_requested() && !stop.stop_requested() &&
                            std::holds_alternative<bool>(ready);
      return;
    }
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
        ticket = request_coordinator_.admit_operation(
            std::move(value), session, reservation,
            [&fatal_errors = fatal_error_reporter_](OperationResponse response, bool mutation) {
              return encode_response(fatal_errors, std::move(response), mutation);
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
    std::stop_callback session_failure(delivery->ended.get_token(),
                                       [&] { ticket->completed.request_stop(); });
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
    if (stop.stop_requested() || delivery->ended.stop_requested()) return;
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
            send_message(stream, final, Clock::now() + 30s, delivery->ended.get_token()))) {
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

void IpcServer::publish(const PushNotification& notification) {
  try {
    // Publication callers serialize changes; this lock also orders independent lock events.
    auto encoded = codec::encode(notification);
    if (!std::holds_alternative<std::string>(encoded) ||
        std::get<std::string>(encoded).size() > max_push_notification_payload_bytes)
      throw std::logic_error("Invalid Backend notification");
    std::lock_guard lock(sessions_mutex_);
    std::erase_if(recipients_, [](const auto& weak) { return weak.expired(); });
    for (auto& weak : recipients_) {
      if (auto recipient = weak.lock()) {
        std::lock_guard queue_lock(recipient->delivery_mutex);
        if (recipient->ended.stop_requested()) continue;
        if (recipient->pending.size() >= limits_.pending_push_notification_capacity) {
          recipient->close(); // No silent loss, coalescing, or unbounded buffering.
          continue;
        }
        recipient->pending.push_back(std::get<std::string>(encoded));
        recipient->queued.request_stop();
      }
    }
  } catch (...) {
    // Fatal producer failure belongs to the lifecycle owner. Keep usable Push
    // connections alive for Report; never alter an authoritative Mutation result.
    fatal_error_reporter_.report(std::current_exception());
  }
}

void IpcServer::serve_push_notification_connection(IpcStream& stream,
                                                   std::shared_ptr<PushDelivery> delivery,
                                                   std::stop_token stop) {
  std::stop_callback closing(stop, [&] { delivery->close(); });
  struct End {
    std::shared_ptr<PushDelivery> state;
    ~End() { state->close(); }
  } end{delivery};
  {
    std::lock_guard lock(sessions_mutex_);
    recipients_.push_back(delivery); // Coverage precedes readiness and every Access capture.
  }
  if (!std::holds_alternative<std::monostate>(send_message(
          stream, {MessageKind::Ready, {}}, Clock::now() + 3s, delivery->ended.get_token())))
    return;
  {
    std::lock_guard lock(delivery->delivery_mutex);
    delivery->paired = true;
    delivery->changed.notify_all();
  }
  std::uint64_t sequence = 0;
  while (!delivery->ended.stop_requested()) {
    std::optional<std::string> pending;
    std::stop_token queued;
    {
      std::lock_guard lock(delivery->delivery_mutex);
      delivery->queued = std::stop_source{};
      queued = delivery->queued.get_token();
      if (!delivery->pending.empty()) pending = delivery->pending.front();
    }
    if (!pending) {
      std::stop_source wake;
      std::stop_callback arrived(queued, [&] { wake.request_stop(); });
      std::stop_callback ended(delivery->ended.get_token(), [&] { wake.request_stop(); });
      auto ready = stream.wait(false, forever, wake.get_token());
      if (std::holds_alternative<IpcError>(ready) || std::get<bool>(ready)) return;
      continue;
    }
    if (++sequence == 0) return;
    auto notification = codec::decode<PushNotification>(*pending);
    auto bytes = codec::encode(PushEnvelope{sequence, std::get<PushNotification>(notification)});
    const auto deadline = Clock::now() + limits_.push_timeout;
    if (!std::holds_alternative<std::monostate>(
            send_message(stream, {MessageKind::Push, payload_bytes(std::get<std::string>(bytes))},
                         deadline, delivery->ended.get_token())))
      return;
    MessageTransport input([](MessageKind kind, std::size_t size) {
      return kind == MessageKind::PushAck && size <= max_push_ack_bytes;
    });
    auto ack = receive_message(stream, input, deadline, delivery->ended.get_token());
    if (!std::holds_alternative<Message>(ack)) return;
    auto receipt = codec::decode<PushReceipt>(payload_text(std::get<Message>(ack).payload));
    if (!std::holds_alternative<PushReceipt>(receipt) ||
        std::get<PushReceipt>(receipt).sequence != sequence)
      return;
    {
      std::lock_guard lock(delivery->delivery_mutex);
      delivery->pending.pop_front();
      delivery->changed.notify_all();
    }
  }
}

bool IpcServer::wait_for_push_delivery() {
  const auto deadline = Clock::now() + limits_.push_timeout;
  std::vector<std::shared_ptr<PushDelivery>> recipients;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& weak : recipients_)
      if (auto recipient = weak.lock()) recipients.push_back(std::move(recipient));
  }
  bool delivered = true;
  for (auto& recipient : recipients) {
    std::unique_lock lock(recipient->delivery_mutex);
    recipient->changed.wait_until(lock, recipient->ended.get_token(), deadline,
                                  [&] { return recipient->pending.empty(); });
    if (!recipient->pending.empty()) {
      delivered = false;
      recipient->close(); // Failed receipt is session-local; never retry or replay.
    }
  }
  return delivered;
}

RequestCoordinator::Result IpcServer::encode_response(FatalErrorReporter& fatal_errors,
                                                      OperationResponse response, bool mutation) {
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
    fatal_errors.report(*error);
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
    const auto fatal_wake = fatal_error_reporter_.wake_token();
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
      auto fatal = fatal_error_reporter_.take_one();
      if (shutdown.stop_requested() || fatal || (served_ && active_ == 0) ||
          (!served_ && active_ == 0 && Clock::now() >= initial_deadline)) {
        stopping_ = true;
        if (fatal) {
          auto error = std::move(*fatal);
          if (auto* ipc = std::get_if<IpcError>(&error)) return *ipc;
          std::rethrow_exception(std::get<std::exception_ptr>(error));
        }
        return std::monostate{};
      }
      deadline = served_ ? forever : initial_deadline;
    }
    // Both signals wake only this wait; reporting holds no connection-state lock.
    std::stop_source progress;
    std::stop_callback connection_changed(changed, [&] { progress.request_stop(); });
    std::stop_callback fatal_changed(fatal_wake, [&] { progress.request_stop(); });
    changed = progress.get_token();
    finished.clear(); // Join completed workers without holding their mutex.
    if (connections_.size() < limits_.connections * 2) {
      auto accepted = ownership_.accept();
      if (auto* error = std::get_if<IpcError>(&accepted)) return *error;
      if (auto& stream = std::get<std::optional<IpcStream>>(accepted); stream) {
        auto entry = std::make_unique<Connection>();
        auto* pointer = entry.get();
        entry->worker = std::jthread(
            [this, pointer, stream = std::move(*stream)](std::stop_token stop) mutable {
              try {
                serve_connection(*pointer, stream, stop);
              } catch (...) {
                fatal_error_reporter_.report(std::current_exception());
              }
              // Preserve a usable stream through Report. Finish wakes this wait;
              // no new requests are received after admission closes.
              {
                std::unique_lock lock(connection_state_mutex_);
                pointer->busy = false;
                workers_changed_.notify_all();
                if (stopping_ && stream.connected() && pointer->delivery &&
                    !pointer->delivery->ended.stop_requested()) {
                  std::stop_source ended;
                  std::stop_callback finish(stop, [&] { ended.request_stop(); });
                  std::stop_callback peer(pointer->delivery->ended.get_token(),
                                          [&] { ended.request_stop(); });
                  std::condition_variable_any closed;
                  closed.wait(lock, ended.get_token(), [] { return false; });
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

void IpcServer::stop_admission() {
  {
    std::lock_guard lock(connection_state_mutex_);
    stopping_ = true;
    notify();
  }
  admission_closed_.request_stop();
  ownership_.stop_listening();
}

void IpcServer::wait_for_responses() {
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
