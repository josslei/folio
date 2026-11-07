#include <folio/application/mutation.hpp>

namespace folio::application {

MutationId MutationId::generate() {
  return MutationId(core::Id::generate());
}

std::optional<MutationId> MutationId::parse(std::string_view text) {
  const auto value = core::Id::parse(text);
  if (!value) return std::nullopt;
  return MutationId(*value);
}

std::string MutationId::to_string() const {
  return value_.to_string();
}

}  // namespace folio::application
