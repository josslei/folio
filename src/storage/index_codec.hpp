#pragma once

#include "core/internal/result.hpp"

#include "storage/metadata_catalog.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace folio::core::storage::index_codec {

enum class IndexCodecErrorCode {
  Malformed,
  UnsupportedVersion,
  LimitExceeded,
  InvalidMetadata
};

struct IndexCodecError {
  IndexCodecErrorCode code;
  std::string path;
  std::string message;
};

// Pure in-memory MessagePack codec. Decoding builds a fresh, persisted catalog;
// errors never mutate an existing catalog or authoritative Entry files.
[[nodiscard]] ::folio::core::internal::Result<std::string, IndexCodecError> encode_index(const MetadataCatalog& catalog);
[[nodiscard]] ::folio::core::internal::Result<MetadataCatalog, IndexCodecError> decode_index(std::string_view bytes);

}  // namespace folio::core::storage::index_codec
