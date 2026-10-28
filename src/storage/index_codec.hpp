#pragma once

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

template <class T>
using Result = std::variant<T, IndexCodecError>;

// Pure in-memory MessagePack codec. Decoding builds a fresh, persisted catalog;
// errors never mutate an existing catalog or authoritative Entry files.
[[nodiscard]] Result<std::string> encode_index(const MetadataCatalog& catalog);
[[nodiscard]] Result<MetadataCatalog> decode_index(std::string_view bytes);

}  // namespace folio::core::storage::index_codec
