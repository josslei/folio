#pragma once

#include "application/internal/metadata_access.hpp"
#include <folio/application/ipc.hpp>

#include <string>
#include <string_view>

namespace folio::application::internal {

// A resource bound on one logical read response, independent of frame size.
// No partial result is returned on overflow. No public pagination is implied.
inline constexpr std::size_t max_access_bytes = 64u * 1024u * 1024u;

core::Result<std::string, IpcError> encode_access_request(const AccessRequest& request);
core::Result<AccessRequest, IpcError> decode_access_request(std::string_view bytes);
core::Result<std::string, IpcError> encode_access_response(const AccessResponse& response);
core::Result<AccessResponse, IpcError> decode_access_response(std::string_view bytes);

// Safe public translation; never forward arbitrary transport diagnostic text.
ApplicationError access_connection_error(const IpcError& error);

}  // namespace folio::application::internal
