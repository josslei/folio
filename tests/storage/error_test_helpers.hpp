#pragma once

#include "storage/normal_repository.hpp"

namespace folio::testing {
// A diagnostic view for assertions; the production result retains its original type.
struct ErrorCode {
  std::variant<core::storage::StorageErrorCode, io::FileErrorCode> value;
  template <class Code> bool operator==(Code code) const {
    const auto* actual = std::get_if<Code>(&value);
    return actual && *actual == code;
  }
};
struct Diagnostic {
  ErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
  bool replacement_completed;
};
template <class Error> Diagnostic diagnostic(const Error& error) {
  return {{error.code}, error.path, error.message, error.system_error, error.replacement_completed};
}
inline Diagnostic diagnostic(const core::storage::RepositoryError& error) {
  return std::visit([](const auto& value) { return diagnostic(value); }, error);
}
inline Diagnostic diagnostic(const core::storage::RepositoryMutationError& error) {
  return diagnostic(error.cause);
}
} // namespace folio::testing
