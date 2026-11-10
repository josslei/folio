#include "backend_connection_handle.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <folio/logging/logging.hpp>
#include <thread>

namespace folio::application::internal {

core::Result<ConnectionSettings, IpcError> connection_settings() {
  auto paths = folio::config::load_configuration();
  if (auto* e = std::get_if<folio::config::ConfigurationError>(&paths)) {
    try {
      SPDLOG_ERROR(
          "Frontend configuration loading failed: ConfigurationError code={} system_error={}",
          static_cast<int>(e->code), e->system_error.value());
    } catch (...) {
    }
    return IpcError{IpcErrorCode::IoError, e->message, e->system_error};
  }
  const auto& effective = std::get<folio::config::EffectiveConfig>(paths);
  logging::set_level(effective.log_level);
  return connection_settings(effective.paths);
}

ApplicationError operation_connection_error(const IpcError& error) {
  auto reason = ErrorReason::IoFailure;
  auto actions = std::vector{ErrorAction::Reconnect, ErrorAction::Dismiss};
  switch (error.code) {
  case IpcErrorCode::Unavailable:
    reason = ErrorReason::BackendUnavailable;
    break;
  case IpcErrorCode::ConnectionLost:
    reason = ErrorReason::ConnectionLost;
    break;
  case IpcErrorCode::Timeout:
    reason = ErrorReason::Timeout;
    break;
  case IpcErrorCode::Protocol:
    reason = ErrorReason::ProtocolError;
    actions = {ErrorAction::Dismiss};
    break;
  case IpcErrorCode::Incompatible:
    reason = ErrorReason::IncompatibleProtocol;
    actions = {ErrorAction::Dismiss};
    break;
  case IpcErrorCode::AccessDenied:
    reason = ErrorReason::AccessDenied;
    actions = {ErrorAction::ResolveExternally, ErrorAction::Dismiss};
    break;
  case IpcErrorCode::IoError:
    break;
  }
  return {ErrorCategory::ConnectionFailure, reason,
          "The Backend connection could not complete the Application operation.",
          std::move(actions)};
}
} // namespace folio::application::internal

namespace folio::application {
using namespace internal;
namespace {
using Clock = std::chrono::steady_clock;

// Public operations are serial on a connection. Only transport/protocol failures
// invalidate it; confirmed Application failures leave the connection usable.
template <class Value>
ApplicationResult<Value> invoke_operation(IpcStream& stream, OperationRequest request,
                                          SubmissionState* submission = nullptr) {
  auto encoded = codec::encode<OperationRequest>(request);
  if (std::holds_alternative<IpcError>(encoded))
    return ApplicationError{ErrorCategory::InvalidRequest,
                            ErrorReason::ResourceLimit,
                            "The Application request exceeds the resource limit.",
                            {ErrorAction::Dismiss}};
  const auto& payload = std::get<std::string>(encoded);
  auto fail = [&](IpcError error) -> ApplicationResult<Value> {
    stream = IpcStream{};
    return operation_connection_error(error);
  };
  if (submission) *submission = SubmissionState::MayHaveBeenSent;
  auto received = exchange_message(stream, {MessageKind::Request, payload_bytes(payload)},
                                   Clock::now() + std::chrono::seconds(30));
  if (auto* error = std::get_if<IpcError>(&received)) return fail(*error);
  auto message = std::get<Message>(std::move(received));
  if (message.kind == MessageKind::Rejected) {
    auto rejection = codec::decode<ApplicationError>(payload_text(message.payload));
    if (auto* error = std::get_if<IpcError>(&rejection)) return fail(*error);
    if (submission) *submission = SubmissionState::Rejected;
    return std::get<ApplicationError>(std::move(rejection));
  }
  if (message.kind != MessageKind::Accepted || !message.payload.empty())
    return fail({IpcErrorCode::Protocol, "Expected Application admission acknowledgment"});
  if (submission) *submission = SubmissionState::Accepted;
  // Acceptance is session-only responsibility, not completion or a liveness
  // guarantee. Queue waiting has no short operation deadline. EOF/protocol
  // failure still invalidates the connection and is not a final Backend result.
  MessageTransport final_transport;
  auto final = receive_message(stream, final_transport, Clock::time_point::max());
  if (auto* error = std::get_if<IpcError>(&final)) return fail(*error);
  message = std::get<Message>(std::move(final));
  if (message.kind != MessageKind::Response)
    return fail({IpcErrorCode::Protocol, "Unexpected Application final response"});
  auto decoded = codec::decode<OperationResponse>(payload_text(message.payload));
  if (auto* error = std::get_if<IpcError>(&decoded)) return fail(*error);
  auto& value = std::get<OperationResponse>(decoded);
  if (auto* error = std::get_if<ApplicationError>(&value)) {
    if constexpr (std::is_same_v<Value, NormalMutationResponse>)
      return fail({IpcErrorCode::Protocol, "Expected a correlated Mutation result"});
    return std::move(*error);
  }
  if (auto* expected = std::get_if<Value>(&value)) {
    if constexpr (std::is_same_v<Value, NormalMutationResponse>) {
      if (expected->mutation_id != std::get<MutationRequest>(request).mutation_id)
        return fail({IpcErrorCode::Protocol, "Mutation response identity mismatch"});
    }
    return std::move(*expected);
  }
  return fail({IpcErrorCode::Protocol, "Wrong Application response type"});
}

template <class Metadata, class Operation>
MutationResult<Metadata> mutate(IpcStream& stream, const MutationId& id,
                                const Operation& operation) {
  auto submission = SubmissionState::NotSent;
  auto response =
      invoke_operation<NormalMutationResponse>(stream, MutationRequest{id, operation}, &submission);
  if (auto* error = std::get_if<ApplicationError>(&response))
    return NoMutationResponse{id, std::move(*error), submission};
  auto confirmed = std::get<NormalMutationResponse>(std::move(response));
  if (auto* failure = std::get_if<MutationFailure>(&confirmed.result))
    return MutationResponse<Metadata>{id, std::move(*failure)};
  auto& result = std::get<MutationValue>(confirmed.result);
  if constexpr (std::is_same_v<Metadata, core::RecordMetadata>) {
    return std::visit(
        [&](auto&& value) -> MutationResult<Metadata> {
          using T = std::decay_t<decltype(value)>;
          if constexpr (!std::is_same_v<T, core::DirectoryMetadata>)
            return MutationResponse<Metadata>{id, Metadata{std::move(value)}};
          stream = IpcStream{};
          return NoMutationResponse{
              id, operation_connection_error({IpcErrorCode::Protocol, "Wrong Move success type"}),
              submission};
        },
        std::move(result));
  } else if (auto* metadata = std::get_if<Metadata>(&result))
    return MutationResponse<Metadata>{id, std::move(*metadata)};
  stream = IpcStream{};
  return NoMutationResponse{
      id, operation_connection_error({IpcErrorCode::Protocol, "Wrong Mutation success type"}),
      submission};
}
} // namespace

core::Result<BackendConnectionHandle, IpcError>
BackendConnectionHandle::connect(const ConnectionSettings& paths,
                                 std::chrono::milliseconds timeout) {
  core::Result<std::filesystem::path, IpcError> endpoint = paths.endpoint;
  if (auto* problem = std::get_if<IpcError>(&endpoint)) return *problem;
  auto connected = IpcStream::connect(std::get<std::filesystem::path>(endpoint));
  if (auto* problem = std::get_if<IpcError>(&connected)) return *problem;
  auto stream = std::get<IpcStream>(std::move(connected));
  auto response = exchange_message(stream, {MessageKind::Hello, {}}, Clock::now() + timeout);
  if (auto* problem = std::get_if<IpcError>(&response)) return *problem;
  auto& message = std::get<Message>(response);
  if (message.kind == MessageKind::Reject)
    return IpcError{IpcErrorCode::Protocol, payload_text(message.payload)};
  if (message.kind != MessageKind::Ready || !message.payload.empty())
    return IpcError{IpcErrorCode::Protocol, "Invalid Backend readiness response"};
  return BackendConnectionHandle(std::move(stream));
}

core::Result<std::monostate, IpcError> BackendConnectionHandle::ping() {
  auto response =
      exchange_message(stream_, {MessageKind::Ping, {}}, Clock::now() + std::chrono::seconds(3));
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

core::Result<BackendConnectionHandle, IpcError>
connect_or_start_backend(const ConnectionSettings& paths, const BackendLauncher& launch,
                         std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  auto next_launch = Clock::time_point::min();
  IpcError last{IpcErrorCode::Unavailable, "Backend is unavailable"};
  while (Clock::now() < deadline) {
    auto budget =
        std::min(std::chrono::milliseconds(3000),
                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
    auto connected = BackendConnectionHandle::connect(paths, budget);
    if (auto* connection = std::get_if<BackendConnectionHandle>(&connected))
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

core::Result<BackendConnectionHandle, IpcError> connect_or_start_backend() {
  auto configured = connection_settings();
  if (auto* problem = std::get_if<IpcError>(&configured))
    return IpcError{IpcErrorCode::IoError, problem->message, problem->system_error};
  return connect_or_start_backend(std::get<ConnectionSettings>(configured), launch_backend);
}
ApplicationResult<ApplicationResponse<BrowseResult>>
BackendConnectionHandle::browse(DirectoryRef dir, RecordOrder order) {
  return invoke_operation<ApplicationResponse<BrowseResult>>(stream_,
                                                             BrowseRequest{std::move(dir), order});
}

ApplicationResult<ApplicationResponse<std::vector<core::RecordMetadata>>>
BackendConnectionHandle::query_records(const RecordQuery& query) {
  return invoke_operation<ApplicationResponse<std::vector<core::RecordMetadata>>>(
      stream_, QueryRecordsRequest{query});
}

ApplicationResult<ApplicationResponse<std::vector<core::DirectoryMetadata>>>
BackendConnectionHandle::list_directories(core::DirectoryKind kind) {
  return invoke_operation<ApplicationResponse<std::vector<core::DirectoryMetadata>>>(
      stream_, ListDirectoriesRequest{kind});
}

ApplicationResult<core::Log::Snapshot> BackendConnectionHandle::load_log(const core::Id& id) {
  return invoke_operation<core::Log::Snapshot>(stream_, LoadLogRequest{id});
}

ApplicationResult<core::Memo::Snapshot> BackendConnectionHandle::load_memo(const core::Id& id) {
  return invoke_operation<core::Memo::Snapshot>(stream_, LoadMemoRequest{id});
}

ApplicationResult<core::Todo::Snapshot> BackendConnectionHandle::load_todo(const core::Id& id) {
  return invoke_operation<core::Todo::Snapshot>(stream_, LoadTodoRequest{id});
}

ApplicationResult<std::monostate> BackendConnectionHandle::acquire_edit(const core::Id& id) {
  return invoke_operation<std::monostate>(stream_, AcquireEditRequest{id});
}
ApplicationResult<std::monostate> BackendConnectionHandle::release_edit(const core::Id& id) {
  return invoke_operation<std::monostate>(stream_, ReleaseEditRequest{id});
}
MutationResult<core::LogMetadata>
BackendConnectionHandle::create(const MutationId& mutation_id, const LogEditableFields& fields,
                                const core::TimestampContext& created,
                                const core::TimestampContext& modified) {
  return mutate<core::LogMetadata>(stream_, mutation_id,
                                   internal::CreateLogRequest{fields, created, modified});
}
MutationResult<core::MemoMetadata>
BackendConnectionHandle::create(const MutationId& mutation_id, const MemoEditableFields& fields,
                                const core::TimestampContext& created,
                                const core::TimestampContext& modified,
                                std::optional<core::Id> directory_id) {
  return mutate<core::MemoMetadata>(
      stream_, mutation_id, internal::CreateMemoRequest{fields, created, modified, directory_id});
}
MutationResult<core::TodoMetadata>
BackendConnectionHandle::create(const MutationId& mutation_id, const TodoEditableFields& fields,
                                const core::TimestampContext& created,
                                const core::TimestampContext& modified,
                                std::optional<core::Id> directory_id) {
  return mutate<core::TodoMetadata>(
      stream_, mutation_id, internal::CreateTodoRequest{fields, created, modified, directory_id});
}
MutationResult<core::LogMetadata>
BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                const LogEditableFields& fields,
                                const core::TimestampContext& modified) {
  return mutate<core::LogMetadata>(stream_, mutation_id,
                                   internal::UpdateLogRequest{entry_id, fields, modified});
}
MutationResult<core::MemoMetadata>
BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                const MemoEditableFields& fields,
                                const core::TimestampContext& modified) {
  return mutate<core::MemoMetadata>(stream_, mutation_id,
                                    internal::UpdateMemoRequest{entry_id, fields, modified});
}
MutationResult<core::TodoMetadata>
BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                const TodoEditableFields& fields,
                                const core::TimestampContext& modified) {
  return mutate<core::TodoMetadata>(stream_, mutation_id,
                                    internal::UpdateTodoRequest{entry_id, fields, modified});
}
MutationResult<core::RecordMetadata>
BackendConnectionHandle::move(const MutationId& mutation_id, const core::Id& entry_id,
                              std::optional<core::Id> destination,
                              const core::TimestampContext& modified) {
  return mutate<core::RecordMetadata>(stream_, mutation_id,
                                      MoveRecordRequest{entry_id, destination, modified});
}
MutationResult<core::DirectoryMetadata>
BackendConnectionHandle::create(const MutationId& mutation_id, core::DirectoryKind kind,
                                const std::string& name) {
  return mutate<core::DirectoryMetadata>(stream_, mutation_id,
                                         internal::CreateDirectoryRequest{kind, name});
}
MutationResult<core::DirectoryMetadata>
BackendConnectionHandle::rename_directory(const MutationId& mutation_id, const core::Id& entry_id,
                                          const std::string& new_name) {
  return mutate<core::DirectoryMetadata>(stream_, mutation_id,
                                         internal::RenameDirectoryRequest{entry_id, new_name});
}
MutationResult<core::TodoMetadata>
BackendConnectionHandle::set_todo_status(const MutationId& mutation_id, const core::Id& entry_id,
                                         core::TodoStatus status,
                                         const core::TimestampContext& modified) {
  return mutate<core::TodoMetadata>(stream_, mutation_id,
                                    internal::SetTodoStatusRequest{entry_id, status, modified});
}

} // namespace folio::application
