#pragma once

#include <variant>

namespace folio::core {

// Shared success-or-error representation. Signatures specify both
// value and error types; callers handle the alternatives explicitly.
template <class T, class E> using Result = std::variant<T, E>;

/// A value accompanied by layer-defined information. Neither member grants
/// authority; each API defines the meaning and ownership of its annotation.
template <class T, class Annotation> struct Annotated {
  T value;
  Annotation annotation;
};

} // namespace folio::core
