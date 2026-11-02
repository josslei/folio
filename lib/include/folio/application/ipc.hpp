#pragma once

#include <folio/core/result.hpp>
#include <folio/config/configuration.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <chrono>
#include <cstddef>
#include <vector>
#include <memory>
#include <optional>
#include <span>

namespace folio::application::internal {
enum class IpcErrorCode {
  Unavailable,
  ConnectionLost,
  Timeout,
  Protocol,
  Incompatible,
  AccessDenied,
  IoError
};

struct IpcError {
  IpcErrorCode code;
  std::string message;
  std::error_code system_error{};
};

struct ConnectionSettings {
  std::filesystem::path endpoint;
};

// Derives settings from resolved paths; does not create or connect a socket.
core::Result<ConnectionSettings, IpcError> connection_settings(
    const config::StoragePaths& paths);
// Convenience overload: loads configuration, then delegates to the overload above.
core::Result<ConnectionSettings, IpcError> connection_settings();

inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t max_message_bytes = 64 * 1024;
enum class MessageKind : std::uint16_t {
  Hello = 1,
  Ready = 2,
  Ping = 3,
  Pong = 4,
  Reject = 5,
  // Internal read protocol. Requests fit one bounded frame; responses may span
  // several frames, with exactly one final ResponseEnd frame.
  AccessRequest = 6,
  ResponseChunk = 7,
  ResponseEnd = 8
};

struct Message {
  MessageKind kind;
  std::vector<std::byte> payload;
  std::uint16_t version = protocol_version;
};

// Explicit conversion at lifecycle text boundaries; payloads are opaque.
std::vector<std::byte> payload_bytes(std::string_view value);
std::string payload_text(std::span<const std::byte> value);

std::string encode_message(const Message& message);
// Incomplete input returns nullopt. Invalid headers are rejected before payload
// allocation.
core::Result<std::optional<Message>, IpcError> decode_message(
    std::string& buffer);

// Move-only private byte stream. Each connection has one reader/writer owner.
// Nonblocking methods return 0 when no progress is possible, errors on disconnect.
class IpcStream {
 public:
  IpcStream();
  ~IpcStream();
  IpcStream(IpcStream&&) noexcept;
  IpcStream& operator=(IpcStream&&) noexcept;
  IpcStream(const IpcStream&) = delete;
  core::Result<std::size_t, IpcError> read(std::span<std::byte> bytes);
  core::Result<std::size_t, IpcError> write(std::span<const std::byte> bytes);
  bool connected() const;
  static core::Result<IpcStream, IpcError> connect(
      const std::filesystem::path& endpoint);

 private:
  struct Native;
  explicit IpcStream(std::unique_ptr<Native> native);
  std::unique_ptr<Native> native_;
  friend class IpcListener;
};

// Caller must hold RepositoryOwnership before listen and until destruction.
// Only the owner may reclaim a stale endpoint; live sockets are never removed.
class IpcListener {
 public:
  ~IpcListener();
  IpcListener(IpcListener&&) noexcept;
  IpcListener& operator=(IpcListener&&) noexcept;
  IpcListener(const IpcListener&) = delete;
  static core::Result<IpcListener, IpcError> listen(
      const std::filesystem::path& endpoint);
  core::Result<std::optional<IpcStream>, IpcError> accept();

 private:
  struct Native;
  explicit IpcListener(std::unique_ptr<Native> native);
  std::unique_ptr<Native> native_;
};
}  // namespace folio::application::internal
