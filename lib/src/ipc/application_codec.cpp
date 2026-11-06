#include <folio/ipc/application_codec.hpp>

namespace folio::application::internal {

core::Result<std::string, IpcError> encode_access_request(const AccessRequest& value) { return codec::encode(value); }
core::Result<AccessRequest, IpcError> decode_access_request(std::string_view bytes) { return codec::decode<AccessRequest>(bytes); }
core::Result<std::string, IpcError> encode_access_response(const AccessResponse& value) { return codec::encode(value); }
core::Result<AccessResponse, IpcError> decode_access_response(std::string_view bytes) { return codec::decode<AccessResponse>(bytes); }

}  // namespace folio::application::internal
