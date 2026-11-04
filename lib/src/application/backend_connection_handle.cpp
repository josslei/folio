#include <folio/application/backend_connection_handle.hpp>
#include "application/internal/application_codec.hpp"
#include "application/internal/message_transport.hpp"
#include <folio/logging/logging.hpp>
#include <thread>

namespace folio::application {
using namespace internal;
namespace {
using Clock = std::chrono::steady_clock;

// Public reads are serial on a connection. Only transport/protocol failures
// invalidate it; confirmed Application failures leave the connection usable.
template<class Value>
ApplicationResult<Value> access(IpcStream& stream, AccessRequest request) {
  auto encoded = encode_access_request(request);
  if (std::holds_alternative<IpcError>(encoded))
    return ApplicationError{ErrorCategory::InvalidRequest, ErrorReason::ResourceLimit,
                            "The query exceeds the request resource limit.", {ErrorAction::Dismiss}};
  const auto& payload = std::get<std::string>(encoded);
  auto fail = [&](IpcError error) -> ApplicationResult<Value> {
    stream = IpcStream{};
    return access_connection_error(error);
  };
  auto received = exchange_message(stream, {MessageKind::Request, payload_bytes(payload)},
                                   Clock::now() + std::chrono::seconds(30));
  if (auto* error = std::get_if<IpcError>(&received)) return fail(*error);
  const auto& message = std::get<Message>(received);
  if (message.kind != MessageKind::Response)
    return fail({IpcErrorCode::Protocol, "Unexpected Application response message"});
  auto decoded = decode_access_response(payload_text(message.payload));
  if (auto* error = std::get_if<IpcError>(&decoded)) return fail(*error);
  auto& value = std::get<AccessResponse>(decoded);
  if (auto* error = std::get_if<ApplicationError>(&value)) return std::move(*error);
  if (auto* expected = std::get_if<Value>(&value)) return std::move(*expected);
  return fail({IpcErrorCode::Protocol, "Wrong Application response type"});
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
  auto response = exchange_message(
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
  auto response = exchange_message(stream_, {MessageKind::Ping, {}},
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
ApplicationResult<ApplicationResponse<BrowseResult>> BackendConnectionHandle::browse(
    DirectoryRef dir, RecordOrder order) {
  return access<ApplicationResponse<BrowseResult>>(stream_, BrowseRequest{std::move(dir), order});
}

ApplicationResult<ApplicationResponse<std::vector<core::RecordMetadata>>> BackendConnectionHandle::query_records(
    const RecordQuery& query) {
  return access<ApplicationResponse<std::vector<core::RecordMetadata>>>(stream_, QueryRecordsRequest{query});
}

ApplicationResult<ApplicationResponse<std::vector<core::DirectoryMetadata>>> BackendConnectionHandle::list_directories(
    core::DirectoryKind kind) {
  return access<ApplicationResponse<std::vector<core::DirectoryMetadata>>>(stream_, ListDirectoriesRequest{kind});
}

ApplicationResult<core::Log::Snapshot> BackendConnectionHandle::load_log(const core::Id& id) {
  return access<core::Log::Snapshot>(stream_, LoadLogRequest{id});
}

ApplicationResult<core::Memo::Snapshot> BackendConnectionHandle::load_memo(const core::Id& id) {
  return access<core::Memo::Snapshot>(stream_, LoadMemoRequest{id});
}

ApplicationResult<core::Todo::Snapshot> BackendConnectionHandle::load_todo(const core::Id& id) {
  return access<core::Todo::Snapshot>(stream_, LoadTodoRequest{id});
}

}  // namespace folio::application
