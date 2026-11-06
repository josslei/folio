#pragma once

#include <folio/ipc/messagepack_codec.hpp>
#include <folio/ipc/ipc.hpp>

#include <string>
#include <string_view>

namespace folio::application::internal {

core::Result<std::string, IpcError> encode_access_request(const AccessRequest& request);
core::Result<AccessRequest, IpcError> decode_access_request(std::string_view bytes);
core::Result<std::string, IpcError> encode_access_response(const AccessResponse& response);
core::Result<AccessResponse, IpcError> decode_access_response(std::string_view bytes);

// No business success/request payload is fixed here. These templates reuse the
// same codec once concrete payloads are approved. NoMutationResponse and
// SubmissionState deliberately have no wire fields.
template<class T>
core::Result<std::string, IpcError> encode_mutation_response(const MutationResponse<T>& response) {
  return codec::encode(response);
}

template<class T>
core::Result<MutationResponse<T>, IpcError> decode_mutation_response(
    std::string_view bytes, const MutationId& expected) {
  auto decoded = codec::decode<MutationResponse<T>>(bytes);
  if (auto* response = std::get_if<MutationResponse<T>>(&decoded);
      response && response->mutation_id != expected)
    return IpcError{IpcErrorCode::Protocol, "Mutation response identity mismatch"};
  return decoded;
}

}  // namespace folio::application::internal
