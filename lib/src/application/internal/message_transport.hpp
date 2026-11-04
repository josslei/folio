#pragma once

#include <folio/application/ipc.hpp>

namespace folio::application::internal {

// Nonblocking progress for complete length-prefixed messages. Partial OS
// transfers stay here; Application handlers only see complete Messages.
// The caller owns the stream and its lifecycle/deadlines. One exchange at a time.
class MessageTransport {
 public:
  core::Result<std::monostate, IpcError> queue(const Message& message);
  core::Result<std::size_t, IpcError> flush(IpcStream& stream);
  core::Result<std::optional<Message>, IpcError> receive(IpcStream& stream);
  bool sending() const { return !output_.empty(); }
  bool receiving() const { return !input_.empty(); }
  std::chrono::steady_clock::time_point last_activity() const { return last_activity_; }

 private:
  std::string input_, output_;
  std::size_t sent_ = 0;
  std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
};

// Synchronous sequential exchange for Frontends. Backend uses the same progress
// methods without blocking other connections. No retry/reconnect/replay occurs.
core::Result<Message, IpcError> exchange_message(
    IpcStream& stream, const Message& request,
    std::chrono::steady_clock::time_point deadline);

}  // namespace folio::application::internal
