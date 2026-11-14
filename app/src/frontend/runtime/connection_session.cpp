#include "frontend/runtime/connection_session.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <utility>
#include <folio/logging/logging.hpp>

namespace folio::application {
using namespace internal;
using Clock = std::chrono::steady_clock;

ConnectionSession::ConnectionSession(IpcStream request, IpcStream stream, std::size_t capacity)
    : capacity_(capacity) {
  request_worker_.stream = std::move(request);
  push_notification_worker_.stream = std::move(stream);
  push_notification_worker_.pending.resize(capacity_);
  push_notification_worker_.thread = std::jthread([this] {
    try {
      receive_push_notifications();
    } catch (...) {
      close();
    }
  });
}

ConnectionSession::~ConnectionSession() {
  close();
  try {
    finish_requests();
  } catch (...) { /* Session is already closed. */
  }
  if (push_notification_worker_.thread.joinable()) push_notification_worker_.thread.join();
}
void ConnectionSession::signal_wakeup() noexcept {
  std::shared_ptr<const std::function<void()>> wakeup;
  {
    std::lock_guard lock(push_notification_worker_.mutex);
    wakeup = push_notification_worker_.wakeup;
  }
  if (wakeup) {
    try {
      (*wakeup)();
    } catch (...) {
      // A broken event-loop signal cannot support reliable delivery.
      ended_.request_stop();
      push_notification_worker_.changed.notify_all();
    }
  }
}
void ConnectionSession::close() {
  {
    std::lock_guard lock(push_notification_worker_.mutex);
    ended_.request_stop();
    push_notification_worker_.session_changed = true;
    push_notification_worker_.changed.notify_all();
  }
  signal_wakeup();
}
void ConnectionSession::request_started() {
  std::lock_guard lock(push_notification_worker_.mutex);
  push_notification_worker_.request_active = true;
}
void ConnectionSession::request_finished() {
  std::lock_guard lock(push_notification_worker_.mutex);
  push_notification_worker_.request_active = false;
  push_notification_worker_.changed.notify_all();
}
std::vector<PushNotification> ConnectionSession::take() {
  // Allocate replacement storage off the receiver's mutex. Swap is constant work;
  // converting the drained batch happens solely on the consuming thread.
  std::vector<std::optional<PushNotification>> batch(capacity_);
  std::size_t count;
  {
    std::lock_guard request_lock(request_worker_.mutex);
    if (request_worker_.outstanding) return {};
    std::lock_guard lock(push_notification_worker_.mutex);
    if (push_notification_worker_.request_active) return {};
    batch.swap(push_notification_worker_.pending);
    count = std::exchange(push_notification_worker_.pending_count, 0);
  }
  std::vector<PushNotification> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    result.push_back(std::move(*batch[i]));
  return result;
}
bool ConnectionSession::wait(std::stop_token stop) {
  std::unique_lock lock(push_notification_worker_.mutex);
  push_notification_worker_.changed.wait(lock, stop, [&] {
    return ended_.stop_requested() || (!push_notification_worker_.request_active &&
                                       push_notification_worker_.pending_count != 0);
  });
  return !push_notification_worker_.request_active && push_notification_worker_.pending_count != 0;
}
void ConnectionSession::receive_push_notifications() {
  auto& stream = push_notification_worker_.stream;
  struct End {
    ConnectionSession& receiver;
    ~End() { receiver.close(); }
  } end{*this};
  std::uint64_t expected = 1;
  while (!ended_.stop_requested()) {
    MessageTransport input([](MessageKind kind, std::size_t size) {
      return kind == MessageKind::Push && size <= max_push_bytes;
    });
    auto message = receive_message(stream, input, Clock::time_point::max(), ended_.get_token());
    if (!std::holds_alternative<Message>(message)) return;
    auto decoded = codec::decode<PushEnvelope>(payload_text(std::get<Message>(message).payload));
    if (!std::holds_alternative<PushEnvelope>(decoded)) return;
    auto event = std::get<PushEnvelope>(std::move(decoded));
    if (expected == 0 || event.sequence != expected++) return;
    {
      std::lock_guard lock(push_notification_worker_.mutex);
      if (push_notification_worker_.pending_count >= capacity_) return;
      push_notification_worker_.pending[push_notification_worker_.pending_count++].emplace(
          std::move(event.notification));
    }
    // Retain before ACK; no UI relevance checks or backlog traversal here.
    auto ack = codec::encode(PushReceipt{event.sequence});
    if (!std::holds_alternative<std::monostate>(
            send_message(stream, {MessageKind::PushAck, payload_bytes(std::get<std::string>(ack))},
                         Clock::now() + std::chrono::seconds(3), ended_.get_token())))
      return;
    {
      std::lock_guard lock(push_notification_worker_.mutex);
      push_notification_worker_.ready = true;
      push_notification_worker_.changed.notify_all();
    }
    signal_wakeup();
  }
}

void ConnectionSession::set_wakeup(std::function<void()> wakeup) {
  auto signal = wakeup ? std::make_shared<const std::function<void()>>(std::move(wakeup)) : nullptr;
  bool pending;
  {
    std::lock_guard lock(push_notification_worker_.mutex);
    push_notification_worker_.wakeup = std::move(signal);
    pending = push_notification_worker_.pending_count != 0 || ended_.stop_requested();
  }
  if (pending) signal_wakeup();
}
void ConnectionSession::start_requests(std::function<void()> wakeup) {
  set_wakeup(std::move(wakeup));
  request_worker_.thread = std::jthread([this](std::stop_token stop) {
    while (true) {
      Request request;
      {
        std::unique_lock lock(request_worker_.mutex);
        if (!request_worker_.changed.wait(lock, stop,
                                          [&] { return bool(request_worker_.pending); }))
          return;
        request = std::exchange(request_worker_.pending, {});
      }
      Completion completion;
      std::exception_ptr failure;
      try {
        completion = request(*this);
      } catch (...) {
        failure = std::current_exception();
        close();
      }
      {
        std::lock_guard lock(request_worker_.mutex);
        request_worker_.completion = completion ? std::move(completion) : Completion([] {});
        request_worker_.failure = failure;
        request_worker_.completion_ready = true;
      }
      signal_wakeup();
      if (failure) return;
    }
  });
}
bool ConnectionSession::submit(Request request) {
  std::lock_guard lock(request_worker_.mutex);
  if (ended_.stop_requested() || !request_worker_.thread.joinable() ||
      request_worker_.thread.get_stop_token().stop_requested() || request_worker_.outstanding ||
      !request)
    return false;
  request_worker_.outstanding = true;
  request_worker_.pending = std::move(request);
  request_worker_.changed.notify_all();
  return true;
}
void ConnectionSession::finish_requests() {
  request_worker_.thread.request_stop();
  request_worker_.changed.notify_all();
  if (request_worker_.thread.joinable()) request_worker_.thread.join();
  process_completion();
}
void ConnectionSession::process_completion() {
  Completion completion;
  std::exception_ptr failure;
  {
    std::lock_guard lock(request_worker_.mutex);
    if (!request_worker_.completion && !request_worker_.failure) return;
    failure = std::exchange(request_worker_.failure, {});
    completion = std::exchange(request_worker_.completion, {});
    request_worker_.completion_ready = false;
    request_worker_.outstanding = false;
  }
  if (failure) std::rethrow_exception(failure);
  try {
    completion();
  } catch (...) {
    close();
    throw;
  }
}
FrontendWork ConnectionSession::take_pending_work() {
  // Consistent lock order: Request slot before retained Push Notifications.
  std::lock_guard requests(request_worker_.mutex);
  std::lock_guard notifications(push_notification_worker_.mutex);
  FrontendWork work;
  work.completion = std::exchange(request_worker_.completion_ready, false);
  work.session_changed = std::exchange(push_notification_worker_.session_changed, false);
  request_worker_.deferred_push |= std::exchange(push_notification_worker_.ready, false);
  if (!request_worker_.outstanding || work.completion)
    work.push_notifications = std::exchange(request_worker_.deferred_push, false);
  return work;
}

core::Result<std::unique_ptr<ConnectionSession>, IpcError>
ConnectionSession::connect(const ConnectionSettings& paths, std::chrono::milliseconds timeout) {
  core::Result<std::filesystem::path, IpcError> endpoint = paths.endpoint;
  if (auto* problem = std::get_if<IpcError>(&endpoint)) return *problem;
  auto connected = IpcStream::connect(std::get<std::filesystem::path>(endpoint));
  if (auto* problem = std::get_if<IpcError>(&connected)) return *problem;
  auto stream = std::get<IpcStream>(std::move(connected));
  auto response = exchange_message(
      stream,
      {MessageKind::Hello,
       payload_bytes(std::get<std::string>(codec::encode(HelloPayload{RequestHello{}})))},
      Clock::now() + timeout);
  if (auto* problem = std::get_if<IpcError>(&response)) return *problem;
  auto& message = std::get<Message>(response);
  if (message.kind == MessageKind::Reject)
    return IpcError{IpcErrorCode::Protocol, payload_text(message.payload)};
  if (message.kind != MessageKind::Ready || !core::Id::parse(payload_text(message.payload)))
    return IpcError{IpcErrorCode::Protocol, "Invalid Backend session pairing response"};
  auto push = IpcStream::connect(paths.endpoint);
  if (auto* error = std::get_if<IpcError>(&push)) return *error;
  auto push_notification_stream = std::get<IpcStream>(std::move(push));
  auto paired = exchange_message(
      push_notification_stream,
      {MessageKind::Hello, payload_bytes(std::get<std::string>(codec::encode(HelloPayload{
                               PushHello{*core::Id::parse(payload_text(message.payload))}})))},
      Clock::now() + timeout);
  if (auto* error = std::get_if<IpcError>(&paired)) return *error;
  const auto& ready = std::get<Message>(paired);
  if (ready.kind != MessageKind::Ready || !ready.payload.empty())
    return IpcError{IpcErrorCode::Protocol, "Invalid Push readiness response"};
  return std::make_unique<ConnectionSession>(std::move(stream), std::move(push_notification_stream),
                                             paths.push_queue_capacity);
}

core::Result<std::unique_ptr<ConnectionSession>, IpcError>
connect_or_start_backend(const ConnectionSettings& paths, const BackendLauncher& launch,
                         std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  auto next_launch = Clock::time_point::min();
  IpcError last{IpcErrorCode::Unavailable, "Backend is unavailable"};
  while (Clock::now() < deadline) {
    auto budget =
        std::min(std::chrono::milliseconds(3000),
                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
    auto connected = ConnectionSession::connect(paths, budget);
    if (auto* connection = std::get_if<std::unique_ptr<ConnectionSession>>(&connected))
      return std::move(*connection);
    last = std::get<IpcError>(std::move(connected));
    if (last.code != IpcErrorCode::Unavailable && last.code != IpcErrorCode::ConnectionLost &&
        last.code != IpcErrorCode::Timeout) {
      try {
        SPDLOG_ERROR("Backend connection failed: IpcError code={}", static_cast<int>(last.code));
      } catch (...) {
        // Logging cannot replace the connection error.
      }
      return last;
    }
    if (Clock::now() >= next_launch) {
      auto started = launch();
      if (auto* problem = std::get_if<ProcessError>(&started)) {
        try {
          SPDLOG_ERROR("Backend launch failed: ProcessError code={} system_error={}",
                       static_cast<int>(problem->code), problem->system_error.value());
        } catch (...) {
          // Preserve the process creation failure.
        }
        return IpcError{IpcErrorCode::IoError, problem->message, problem->system_error};
      }
      next_launch = Clock::now() + std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  try {
    SPDLOG_ERROR("Backend connection/startup deadline expired: IpcError code={}",
                 static_cast<int>(last.code));
  } catch (...) {
    // Preserve the original timeout result.
  }
  return IpcError{IpcErrorCode::Timeout, "Backend startup deadline expired: " + last.message,
                  last.system_error};
}

core::Result<std::unique_ptr<ConnectionSession>, IpcError> connect_or_start_backend() {
  auto configured = connection_settings();
  if (auto* problem = std::get_if<IpcError>(&configured))
    return IpcError{IpcErrorCode::IoError, problem->message, problem->system_error};
  return connect_or_start_backend(std::get<ConnectionSettings>(configured), launch_backend);
}

} // namespace folio::application
