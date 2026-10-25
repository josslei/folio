#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace folio::core {

void ensure_sodium_initialized();

enum class Privacy { Normal, Private };

using Timestamp = std::chrono::system_clock::time_point;

struct TimestampContext {
  Timestamp timestamp;
  std::string timezone;  // Original timezone identifier.
  std::optional<std::string> location;  // UTF-8.
};

struct DateContext {
  std::chrono::year_month_day date;
  std::string timezone;  // Original timezone identifier.
  std::optional<std::string> location;  // UTF-8.
};

class Id {
 public:
  [[nodiscard]] static Id generate();
  [[nodiscard]] static std::optional<Id> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;

  bool operator==(const Id&) const = default;

 private:
  explicit Id(std::array<std::uint8_t, 16> bytes) : bytes_(bytes) {}

  std::array<std::uint8_t, 16> bytes_;
};

struct Tag {
  std::string value;  // UTF-8.

  bool operator==(const Tag&) const = default;
};

}  // namespace folio::core
