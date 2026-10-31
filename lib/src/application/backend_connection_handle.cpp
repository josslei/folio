#include <folio/application/backend_connection_handle.hpp>
#include <folio/logging/logging.hpp>
#include <array>
#include <thread>

namespace folio::application {
using namespace internal;
namespace {
using Clock = std::chrono::steady_clock;

core::Result<Message, IpcError> exchange(IpcStream& stream, Message message,
                                                   Clock::time_point deadline) {
  auto outgoing = encode_message(message);
  std::size_t offset = 0;
  std::string incoming;
  std::array<std::byte, 4096> bytes{};
  while (Clock::now() < deadline) {
    if (offset < outgoing.size()) {
      auto sent = stream.write(
          std::as_bytes(std::span(outgoing.data(), outgoing.size())).subspan(offset));
      if (auto* problem = std::get_if<IpcError>(&sent))
        return *problem;
      offset += std::get<std::size_t>(sent);
    } else {
      auto read = stream.read(bytes);
      if (auto* problem = std::get_if<IpcError>(&read))
        return *problem;
      incoming += payload_text(std::span(bytes).first(std::get<std::size_t>(read)));
      auto decoded = decode_message(incoming);
      if (auto* problem = std::get_if<IpcError>(&decoded))
        return *problem;
      auto& response = std::get<std::optional<Message>>(decoded);
      if (response) {
        if (!incoming.empty())
          return IpcError{IpcErrorCode::Protocol, "Unexpected trailing response"};
        return std::move(*response);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return IpcError{IpcErrorCode::Timeout, "Backend handshake/response timed out"};
}
}  // namespace

core::Result<BackendConnectionHandle, IpcError> BackendConnectionHandle::connect(
    const ConnectionSettings& paths, std::chrono::milliseconds timeout) {
  core::Result<std::filesystem::path, IpcError> endpoint = paths.endpoint;
  if (auto* problem = std::get_if<IpcError>(&endpoint))
    return *problem;
  auto connected = IpcStream::connect(std::get<std::filesystem::path>(endpoint));
  if (auto* problem = std::get_if<IpcError>(&connected))
    return *problem;
  auto stream = std::get<IpcStream>(std::move(connected));
  auto response = exchange(
      stream, {MessageKind::Hello, {}},
      Clock::now() + timeout);
  if (auto* problem = std::get_if<IpcError>(&response))
    return *problem;
  auto& message = std::get<Message>(response);
  if (message.kind == MessageKind::Reject)
    return IpcError{IpcErrorCode::Protocol, payload_text(message.payload)};
  if (message.kind != MessageKind::Ready ||
      !message.payload.empty())
    return IpcError{IpcErrorCode::Protocol,
                    "Invalid Backend readiness response"};
  return BackendConnectionHandle(std::move(stream));
}

core::Result<std::monostate, IpcError> BackendConnectionHandle::ping() {
  auto response = exchange(stream_, {MessageKind::Ping, {}},
                           Clock::now() + std::chrono::seconds(3));
  if (auto* problem = std::get_if<IpcError>(&response)) {
    stream_ = IpcStream{};
    return *problem;
  }
  auto& message = std::get<Message>(response);
  if (message.kind != MessageKind::Pong || !message.payload.empty()) {
    stream_ = IpcStream{};
    return IpcError{IpcErrorCode::Protocol, "Invalid Backend ping response"};
  }
  return std::monostate{};
}

core::Result<BackendConnectionHandle, IpcError> connect_or_start_backend(
    const ConnectionSettings& paths, const BackendLauncher& launch,
    std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  auto next_launch = Clock::time_point::min();
  IpcError last{IpcErrorCode::Unavailable, "Backend is unavailable"};
  while (Clock::now() < deadline) {
    auto budget = std::min(
        std::chrono::milliseconds(3000),
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
    auto connected = BackendConnectionHandle::connect(paths, budget);
    if (auto* connection = std::get_if<BackendConnectionHandle>(&connected))
      return std::move(*connection);
    last = std::get<IpcError>(std::move(connected));
    if (last.code != IpcErrorCode::Unavailable &&
        last.code != IpcErrorCode::ConnectionLost && last.code != IpcErrorCode::Timeout) {
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
  return IpcError{IpcErrorCode::Timeout,
                  "Backend startup deadline expired: " + last.message,
                  last.system_error};
}

core::Result<BackendConnectionHandle, IpcError> connect_or_start_backend() {
  auto configured = connection_settings();
  if (auto* problem = std::get_if<IpcError>(&configured))
    return IpcError{IpcErrorCode::IoError, problem->message, problem->system_error};
  return connect_or_start_backend(std::get<ConnectionSettings>(configured),
                                  launch_backend);
}
}  // namespace folio::application
