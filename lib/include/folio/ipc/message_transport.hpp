#pragma once

#include <folio/ipc/ipc.hpp>
#include <functional>

namespace folio::application::internal {

// Nonblocking progress for complete length-prefixed messages. Partial OS
// transfers stay here; Application handlers only see complete Messages.
// The caller owns the stream and its lifecycle/deadlines. One exchange at a time.
class MessageTransport {
 public:
  // Optional Backend resource admission before payload buffering. Called once
  // after validating a header; false rejects reception without allocating it.
  explicit MessageTransport(std::function<bool(MessageKind, std::size_t)> reserve = {})
      : reserve_(std::move(reserve)) {}
  core::Result<std::monostate, IpcError> queue(const Message& message);
  core::Result<std::size_t, IpcError> flush(IpcStream& stream);
  core::Result<std::optional<Message>, IpcError> receive(IpcStream& stream);
  bool sending() const { return !output_.empty(); }
  bool receiving() const { return !input_.empty(); }
  std::chrono::steady_clock::time_point last_activity() const { return last_activity_; }

 private:
  std::function<bool(MessageKind, std::size_t)> reserve_;
  bool reserved_ = false;
  std::string input_, output_;
  std::size_t sent_ = 0;
  std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
};

core::Result<std::monostate, IpcError> send_message(
    IpcStream& stream, const Message& message,
    std::chrono::steady_clock::time_point deadline, std::stop_token wake = {});
core::Result<Message, IpcError> receive_message(
    IpcStream& stream, MessageTransport& transport,
    std::chrono::steady_clock::time_point deadline, std::stop_token wake = {});

// Synchronous sequential exchange for Frontends. Backend uses the same progress
// methods without blocking other connections. No retry/reconnect/replay occurs.
core::Result<Message, IpcError> exchange_message(
    IpcStream& stream, const Message& request,
    std::chrono::steady_clock::time_point deadline);

}  // namespace folio::application::internal
