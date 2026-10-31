#pragma once

#include <folio/application/ipc.hpp>
#include <folio/application/backend.hpp>
#include <functional>
#include <utility>

namespace folio::application {
// Owns an established Backend connection for TUI, CLI, and other Frontends.
// Destruction closes the connection; moving transfers ownership.
// Application CRUD/Query contracts remain deferred. No automatic
// reconnect/replay after handshake. One caller owns request/response access.
class BackendConnectionHandle {
 public:
  BackendConnectionHandle(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle& operator=(BackendConnectionHandle&&) noexcept = default;
  BackendConnectionHandle(const BackendConnectionHandle&) = delete;

  bool connected() const {
    return stream_.connected();
  }

  core::Result<std::monostate, internal::IpcError> ping();
  static core::Result<BackendConnectionHandle, internal::IpcError> connect(
      const internal::ConnectionSettings& paths,
      std::chrono::milliseconds timeout = std::chrono::seconds(3));

 private:
  explicit BackendConnectionHandle(internal::IpcStream stream) : stream_(std::move(stream)) {
  }

  internal::IpcStream stream_;
};

// Injectable launch operation permits controlled process tests without private
// production CLI path arguments. Production uses launch_backend().
using BackendLauncher = std::function<core::Result<LaunchedProcess, ProcessError>()>;

core::Result<BackendConnectionHandle, internal::IpcError> connect_or_start_backend(
    const internal::ConnectionSettings& paths, const BackendLauncher& launch,
    std::chrono::milliseconds timeout = std::chrono::seconds(15));

core::Result<BackendConnectionHandle, internal::IpcError> connect_or_start_backend();

}  // namespace folio::application
