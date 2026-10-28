#include "storage/binary_io.hpp"

#include <array>
#include <fstream>

namespace folio::core::storage {

BinaryReadResult<std::string> read_binary_file(const std::filesystem::path& path,
                                               std::size_t max_bytes) {
  if (path.empty() || path.native().find(std::filesystem::path::value_type{}) !=
                          std::filesystem::path::string_type::npos)
    return BinaryReadError{BinaryReadErrorCode::IoError, path,
                           "Invalid binary file path", std::make_error_code(std::errc::invalid_argument)};
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return BinaryReadError{BinaryReadErrorCode::IoError, path,
                                 "Could not measure binary file", ec};
  if (size > max_bytes)
    return BinaryReadError{BinaryReadErrorCode::LimitExceeded, path,
                           "Binary file exceeds byte limit", {}};
  std::ifstream input(path, std::ios::binary);
  if (!input) return BinaryReadError{BinaryReadErrorCode::IoError, path,
                                     "Could not open binary file", {}};
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(size));
  std::array<char, 64u * 1024u> chunk{};
  while (input) {
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = input.gcount();
    if (count > 0) {
      if (static_cast<std::size_t>(count) > max_bytes - bytes.size())
        return BinaryReadError{BinaryReadErrorCode::LimitExceeded, path,
                               "Binary file exceeds byte limit", {}};
      bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
  }
  if (input.bad() || (input.fail() && !input.eof()))
    return BinaryReadError{BinaryReadErrorCode::IoError, path,
                           "Could not read binary file", {}};
  return bytes;
}

}  // namespace folio::core::storage
