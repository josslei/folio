#include "frontend/runtime/connection_session.hpp"
#include "frontend/runtime/backend_connection_handle.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <folio/logging/logging.hpp>
#include <thread>
#include <condition_variable>
#include <mutex>

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
  auto settings = connection_settings(effective.paths);
  if (auto* value = std::get_if<ConnectionSettings>(&settings))
    value->push_queue_capacity = effective.frontend_push_queue_capacity;
  return settings;
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
using Clock = std::chrono::steady_clock;

namespace {
using Clock = std::chrono::steady_clock;

// Public operations are serial on a connection. Only transport/protocol failures
// invalidate it; confirmed Application failures leave the connection usable.
template <class Value, class Receiver>
AccessResult<Value> invoke_operation(IpcStream& stream, Receiver& receiver,
                                     OperationRequest request,
                                     SubmissionState* submission = nullptr) {
  receiver.request_started();
  struct RequestBarrier {
    Receiver& receiver;
    ~RequestBarrier() { receiver.request_finished(); }
  } barrier{receiver};
  auto encoded = codec::encode<OperationRequest>(request);
  if (std::holds_alternative<IpcError>(encoded))
    return ApplicationError{ErrorCategory::InvalidRequest,
                            ErrorReason::ResourceLimit,
                            "The Application request exceeds the resource limit.",
                            {ErrorAction::Dismiss}};
  const auto& payload = std::get<std::string>(encoded);
  auto fail = [&](IpcError error) -> AccessResult<Value> {
    receiver.close();
    stream = IpcStream{};
    return operation_connection_error(error);
  };
  if (submission) *submission = SubmissionState::MayHaveBeenSent;
  auto received = exchange_message(stream, {MessageKind::Request, payload_bytes(payload)},
                                   Clock::now() + std::chrono::seconds(30), receiver.stop_token());
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
  auto final =
      receive_message(stream, final_transport, Clock::time_point::max(), receiver.stop_token());
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

template <class Metadata, class Operation, class Receiver>
MutationResult<Metadata> mutate(IpcStream& stream, Receiver& receiver, const MutationId& id,
                                const Operation& operation) {
  auto submission = SubmissionState::NotSent;
  auto response = invoke_operation<NormalMutationResponse>(
      stream, receiver, MutationRequest{id, operation}, &submission);
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
          receiver.close();
          stream = IpcStream{};
          return NoMutationResponse{
              id, operation_connection_error({IpcErrorCode::Protocol, "Wrong Move success type"}),
              submission};
        },
        std::move(result));
  } else if (auto* metadata = std::get_if<Metadata>(&result))
    return MutationResponse<Metadata>{id, std::move(*metadata)};
  receiver.close();
  stream = IpcStream{};
  return NoMutationResponse{
      id, operation_connection_error({IpcErrorCode::Protocol, "Wrong Mutation success type"}),
      submission};
}
} // namespace

namespace {
// Only the handle builds the worker-to-main-thread handoff.
template <class Execute, class Completed>
ConnectionSession::Request request_with_completion(Execute execute, Completed completed) {
  return
      [execute = std::move(execute), completed = std::move(completed)](ConnectionSession& session) {
        auto result = execute(session);
        return [completed = std::move(completed), result = std::move(result)]() mutable {
          completed(std::move(result));
        };
      };
}
} // namespace
BackendConnectionHandle::BackendConnectionHandle(std::unique_ptr<ConnectionSession> session,
                                                 std::function<void()> wakeup)
    : session_(std::move(session)) {
  session_->start_requests(std::move(wakeup));
}
BackendConnectionHandle::~BackendConnectionHandle() {
  try {
    finish_requests();
  } catch (...) {
    session_->close();
  }
  session_->set_wakeup({});
}
void BackendConnectionHandle::finish_requests() {
  session_->finish_requests();
}
void BackendConnectionHandle::process_completion() {
  session_->process_completion();
}
std::vector<PushNotification> BackendConnectionHandle::take_push_notifications() {
  return session_->take();
}
bool BackendConnectionHandle::connected() const {
  return session_->connected();
}
FrontendWork BackendConnectionHandle::take_pending_work() {
  return session_->take_pending_work();
}
bool BackendConnectionHandle::ping(
    CompletionCallback<core::Result<std::monostate, internal::IpcError>> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [](ConnectionSession& session) -> core::Result<std::monostate, IpcError> {
        auto response =
            exchange_message(session.request_connection(), {MessageKind::Ping, {}},
                             Clock::now() + std::chrono::seconds(3), session.stop_token());
        if (auto* problem = std::get_if<IpcError>(&response)) {
          session.close();
          session.request_connection() = IpcStream{};
          return *problem;
        }
        auto& message = std::get<Message>(response);
        if (message.kind != MessageKind::Pong || !message.payload.empty()) {
          session.close();
          session.request_connection() = IpcStream{};
          return IpcError{IpcErrorCode::Protocol, "Invalid Backend ping response"};
        }
        return std::monostate{};
      },
      std::move(completed)));
}
bool BackendConnectionHandle::browse(DirectoryRef dir, RecordOrder order,
                                     AnnotatedAccessCallback<BrowseResult> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [dir, order](ConnectionSession& session) {
        return invoke_operation<Annotated<BrowseResult>>(session.request_connection(), session,
                                                         BrowseRequest{std::move(dir), order});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::query_records(const RecordQuery& query,
                                            AnnotatedAccessCallback<RecordMetadataList> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [query](ConnectionSession& session) {
        return invoke_operation<Annotated<RecordMetadataList>>(session.request_connection(),
                                                               session, QueryRecordsRequest{query});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::list_directories(
    core::DirectoryKind kind, AnnotatedAccessCallback<DirectoryMetadataList> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [kind](ConnectionSession& session) {
        return invoke_operation<Annotated<DirectoryMetadataList>>(
            session.request_connection(), session, ListDirectoriesRequest{kind});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::check_consistency(DirectoryRef scope,
                                                AccessCallback<ConsistencyCheckResult> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [scope](ConnectionSession& session) {
        return invoke_operation<ConsistencyCheckResult>(session.request_connection(), session,
                                                        CheckConsistencyRequest{std::move(scope)});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::refresh(RefreshScope scope,
                                      AccessCallback<std::monostate> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [scope](ConnectionSession& session) {
        return invoke_operation<std::monostate>(session.request_connection(), session,
                                                RefreshRequest{std::move(scope)});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::load_log(const core::Id& id,
                                       AnnotatedAccessCallback<core::Log::Snapshot> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [id](ConnectionSession& session) {
        return invoke_operation<Annotated<core::Log::Snapshot>>(session.request_connection(),
                                                                session, LoadLogRequest{id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::load_memo(const core::Id& id,
                                        AnnotatedAccessCallback<core::Memo::Snapshot> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [id](ConnectionSession& session) {
        return invoke_operation<Annotated<core::Memo::Snapshot>>(session.request_connection(),
                                                                 session, LoadMemoRequest{id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::load_todo(const core::Id& id,
                                        AnnotatedAccessCallback<core::Todo::Snapshot> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [id](ConnectionSession& session) {
        return invoke_operation<Annotated<core::Todo::Snapshot>>(session.request_connection(),
                                                                 session, LoadTodoRequest{id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::acquire_edit(const core::Id& id,
                                           AccessCallback<std::monostate> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [id](ConnectionSession& session) {
        return invoke_operation<std::monostate>(session.request_connection(), session,
                                                AcquireEditRequest{id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::release_edit(const core::Id& id,
                                           AccessCallback<std::monostate> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [id](ConnectionSession& session) {
        return invoke_operation<std::monostate>(session.request_connection(), session,
                                                ReleaseEditRequest{id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::create(const MutationId& mutation_id, const LogEditableFields& fields,
                                     const core::TimestampContext& created,
                                     const core::TimestampContext& modified,
                                     MutationCallback<core::LogMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, fields, created, modified](ConnectionSession& session) {
        return mutate<core::LogMetadata>(session.request_connection(), session, mutation_id,
                                         internal::CreateLogRequest{fields, created, modified});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::create(const MutationId& mutation_id,
                                     const MemoEditableFields& fields,
                                     const core::TimestampContext& created,
                                     const core::TimestampContext& modified,
                                     std::optional<core::Id> directory_id,
                                     MutationCallback<core::MemoMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, fields, created, modified, directory_id](ConnectionSession& session) {
        return mutate<core::MemoMetadata>(
            session.request_connection(), session, mutation_id,
            internal::CreateMemoRequest{fields, created, modified, directory_id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::create(const MutationId& mutation_id,
                                     const TodoEditableFields& fields,
                                     const core::TimestampContext& created,
                                     const core::TimestampContext& modified,
                                     std::optional<core::Id> directory_id,
                                     MutationCallback<core::TodoMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, fields, created, modified, directory_id](ConnectionSession& session) {
        return mutate<core::TodoMetadata>(
            session.request_connection(), session, mutation_id,
            internal::CreateTodoRequest{fields, created, modified, directory_id});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                     const LogEditableFields& fields,
                                     const core::TimestampContext& modified,
                                     MutationCallback<core::LogMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, fields, modified](ConnectionSession& session) {
        return mutate<core::LogMetadata>(session.request_connection(), session, mutation_id,
                                         internal::UpdateLogRequest{entry_id, fields, modified});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                     const MemoEditableFields& fields,
                                     const core::TimestampContext& modified,
                                     MutationCallback<core::MemoMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, fields, modified](ConnectionSession& session) {
        return mutate<core::MemoMetadata>(session.request_connection(), session, mutation_id,
                                          internal::UpdateMemoRequest{entry_id, fields, modified});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::update(const MutationId& mutation_id, const core::Id& entry_id,
                                     const TodoEditableFields& fields,
                                     const core::TimestampContext& modified,
                                     MutationCallback<core::TodoMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, fields, modified](ConnectionSession& session) {
        return mutate<core::TodoMetadata>(session.request_connection(), session, mutation_id,
                                          internal::UpdateTodoRequest{entry_id, fields, modified});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::move(const MutationId& mutation_id, const core::Id& entry_id,
                                   std::optional<core::Id> destination,
                                   const core::TimestampContext& modified,
                                   MutationCallback<core::RecordMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, destination, modified](ConnectionSession& session) {
        return mutate<core::RecordMetadata>(session.request_connection(), session, mutation_id,
                                            MoveRecordRequest{entry_id, destination, modified});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::create(const MutationId& mutation_id, core::DirectoryKind kind,
                                     const std::string& name,
                                     MutationCallback<core::DirectoryMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, kind, name](ConnectionSession& session) {
        return mutate<core::DirectoryMetadata>(session.request_connection(), session, mutation_id,
                                               internal::CreateDirectoryRequest{kind, name});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::rename_directory(
    const MutationId& mutation_id, const core::Id& entry_id, const std::string& new_name,
    MutationCallback<core::DirectoryMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, new_name](ConnectionSession& session) {
        return mutate<core::DirectoryMetadata>(
            session.request_connection(), session, mutation_id,
            internal::RenameDirectoryRequest{entry_id, new_name});
      },
      std::move(completed)));
}
bool BackendConnectionHandle::set_todo_status(const MutationId& mutation_id,
                                              const core::Id& entry_id, core::TodoStatus status,
                                              const core::TimestampContext& modified,
                                              MutationCallback<core::TodoMetadata> completed) {
  if (!completed) return false;
  return session_->submit(request_with_completion(
      [mutation_id, entry_id, status, modified](ConnectionSession& session) {
        return mutate<core::TodoMetadata>(
            session.request_connection(), session, mutation_id,
            internal::SetTodoStatusRequest{entry_id, status, modified});
      },
      std::move(completed)));
}
} // namespace folio::application
