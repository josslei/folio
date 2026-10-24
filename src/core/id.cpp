#include "folio/core/types.hpp"

#include <uuid.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace folio::core {
namespace {

std::array<std::uint8_t, 16> to_bytes(const uuids::uuid& uuid) {
  std::array<std::uint8_t, 16> bytes;
  std::transform(uuid.as_bytes().begin(), uuid.as_bytes().end(), bytes.begin(),
                 [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
  return bytes;
}

bool is_canonical_uuid_string(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }

  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return false;
      }
    } else if (!((text[i] >= '0' && text[i] <= '9') ||
                 (text[i] >= 'a' && text[i] <= 'f') ||
                 (text[i] >= 'A' && text[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

}  // namespace

Id Id::generate() {
  ensure_sodium_initialized();

  std::array<std::uint8_t, 16> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  return Id(bytes);
}

std::optional<Id> Id::parse(std::string_view text) {
  if (!is_canonical_uuid_string(text)) {
    return std::nullopt;
  }

  const auto uuid = uuids::uuid::from_string(text);
  if (!uuid || uuid->version() != uuids::uuid_version::random_number_based ||
      uuid->variant() != uuids::uuid_variant::rfc) {
    return std::nullopt;
  }
  return Id(to_bytes(*uuid));
}

std::string Id::to_string() const {
  return uuids::to_string(uuids::uuid(bytes_));
}

}  // namespace folio::core
