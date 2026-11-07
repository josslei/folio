#pragma once

#include <folio/ipc/messagepack_codec.hpp>
#include <folio/ipc/ipc.hpp>

#include <string>
#include <string_view>

namespace folio::application::internal {


// Confirmed mutation envelopes use the same codec as Query and concrete Normal
// requests/results. NoMutationResponse and SubmissionState are local Frontend
// observations and deliberately have no wire fields.
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
