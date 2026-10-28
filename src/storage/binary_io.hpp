#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>

namespace folio::core::storage {

enum class BinaryReadErrorCode { IoError, LimitExceeded };

struct BinaryReadError {
  BinaryReadErrorCode code;
  std::filesystem::path path;
  std::string message;
  std::error_code system_error;
};

template <class T>
using BinaryReadResult = std::variant<T, BinaryReadError>;

// Reads opaque bytes, including embedded NUL. The caller supplies the cap and
// owns path confinement and symlink policy.
[[nodiscard]] BinaryReadResult<std::string> read_binary_file(
    const std::filesystem::path& path, std::size_t max_bytes);

}  // namespace folio::core::storage
