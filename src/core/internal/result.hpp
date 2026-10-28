#pragma once

#include <variant>

namespace folio::core::internal {

// Shared internal success-or-error representation. Signatures specify both
// value and error types; callers handle the alternatives explicitly.
template <class T, class E>
using Result = std::variant<T, E>;

}  // namespace folio::core::internal
