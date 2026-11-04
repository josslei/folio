#include "application/internal/application_codec.hpp"

namespace folio::application::internal {

core::Result<std::string, IpcError> encode_access_request(const AccessRequest& value) { return codec::encode(value); }
core::Result<AccessRequest, IpcError> decode_access_request(std::string_view bytes) { return codec::decode<AccessRequest>(bytes); }
core::Result<std::string, IpcError> encode_access_response(const AccessResponse& value) { return codec::encode(value); }
core::Result<AccessResponse, IpcError> decode_access_response(std::string_view bytes) { return codec::decode<AccessResponse>(bytes); }

ApplicationError access_connection_error(const IpcError& error) {
  auto reason = ErrorReason::IoFailure;
  auto actions = std::vector{ErrorAction::Reconnect, ErrorAction::Dismiss};
  switch (error.code) {
    case IpcErrorCode::Unavailable: reason = ErrorReason::BackendUnavailable; break;
    case IpcErrorCode::ConnectionLost: reason = ErrorReason::ConnectionLost; break;
    case IpcErrorCode::Timeout: reason = ErrorReason::Timeout; break;
    case IpcErrorCode::Protocol: reason = ErrorReason::ProtocolError; actions = {ErrorAction::Dismiss}; break;
    case IpcErrorCode::Incompatible: reason = ErrorReason::IncompatibleProtocol; actions = {ErrorAction::Dismiss}; break;
    case IpcErrorCode::AccessDenied: reason = ErrorReason::AccessDenied; actions = {ErrorAction::ResolveExternally, ErrorAction::Dismiss}; break;
    case IpcErrorCode::IoError: break;
  }
  return {ErrorCategory::ConnectionFailure, reason, "The Backend connection could not complete the read operation.", std::move(actions)};
}
}  // namespace folio::application::internal
