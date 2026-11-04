#pragma once

#include <folio/core/types.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace folio::application {

// Identifies one mutation intention, not an Entry or Draft. Correlation alone
// provides no deduplication, receipt, or cross-restart execution guarantee.
class MutationId {
 public:
  [[nodiscard]] static MutationId generate();
  [[nodiscard]] static std::optional<MutationId> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;

  bool operator==(const MutationId&) const = default;

 private:
  explicit MutationId(core::Id value) : value_(value) {}

  core::Id value_;
};

}  // namespace folio::application
