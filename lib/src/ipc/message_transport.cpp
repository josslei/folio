#include <folio/ipc/message_transport.hpp>

#include <algorithm>
#include <array>

namespace folio::application::internal {

core::Result<std::monostate, IpcError> MessageTransport::queue(const Message& message) {
  if (sending()) return IpcError{IpcErrorCode::Protocol, "A message is already being sent"};
  if (message.payload.size() > max_application_bytes)
    return IpcError{IpcErrorCode::Protocol, "Message exceeds resource limit"};
  output_ = encode_message(message);
  sent_ = 0;
  last_activity_ = std::chrono::steady_clock::now();
  return std::monostate{};
}

core::Result<std::size_t, IpcError> MessageTransport::flush(IpcStream& stream) {
  if (!sending()) return std::size_t{0};
  // A bounded OS transfer buffer, not a protocol frame boundary. A connection
  // worker waits for writability only when its stream cannot make progress.
  const auto size = std::min(output_.size() - sent_, std::size_t{65536});
  auto written =
      stream.write(std::as_bytes(std::span(output_.data(), output_.size())).subspan(sent_, size));
  if (auto* error = std::get_if<IpcError>(&written)) return *error;
  const auto count = std::get<std::size_t>(written);
  sent_ += count;
  if (count) last_activity_ = std::chrono::steady_clock::now();
  if (sent_ == output_.size()) {
    std::string{}.swap(output_);
    sent_ = 0;
  }
  return count;
}

core::Result<std::optional<Message>, IpcError> MessageTransport::receive(IpcStream& stream) {
  std::array<std::byte, 65536> bytes{};
  // Read/validate the header before accumulating any payload. Do not allocate
  // the advertised message length up front; grow only as actual bytes arrive.
  const auto size = input_.size() < 8 ? 8 - input_.size() : bytes.size();
  auto read = stream.read(std::span(bytes).first(size));
  if (auto* error = std::get_if<IpcError>(&read)) return *error;
  const auto count = std::get<std::size_t>(read);
  if (count) {
    last_activity_ = std::chrono::steady_clock::now();
    input_.append(reinterpret_cast<const char*>(bytes.data()), count);
  }
  // Validate the header before asking the owner to reserve bounded resources.
  if (input_.size() >= 8 && !reserved_) {
    auto header = input_.substr(0, 8);
    auto checked = decode_message(header);
    if (auto* error = std::get_if<IpcError>(&checked)) return *error;
  }
  if (!reserved_ && input_.size() >= 8) {
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(input_[index]); };
    const auto kind = static_cast<MessageKind>((byte(6) << 8) | byte(7));
    const std::size_t size = (std::size_t{byte(0)} << 24) | (std::size_t{byte(1)} << 16) |
                             (std::size_t{byte(2)} << 8) | byte(3);
    if (reserve_ && !reserve_(kind, size))
      return IpcError{IpcErrorCode::Protocol, "Message rejected before payload buffering"};
    reserved_ = true;
  }
  auto decoded = decode_message(input_);
  if (auto* error = std::get_if<IpcError>(&decoded)) return *error;
  auto& message = std::get<std::optional<Message>>(decoded);
  if (message) {
    if (!input_.empty())
      return IpcError{IpcErrorCode::Protocol, "Unexpected trailing message bytes"};
    std::string{}.swap(input_);
    reserved_ = false;
  }
  return decoded;
}

core::Result<std::monostate, IpcError> send_message(IpcStream& stream, const Message& message,
                                                    std::chrono::steady_clock::time_point deadline,
                                                    std::stop_token wake) {
  MessageTransport transport;
  auto queued = transport.queue(message);
  if (auto* error = std::get_if<IpcError>(&queued)) return *error;
  while (transport.sending()) {
    if (wake.stop_requested())
      return IpcError{IpcErrorCode::ConnectionLost, "IPC wait interrupted"};
    if (std::chrono::steady_clock::now() >= deadline)
      return IpcError{IpcErrorCode::Timeout, "IPC send timed out"};
    auto sent = transport.flush(stream);
    if (auto* error = std::get_if<IpcError>(&sent)) return *error;
    if (std::get<std::size_t>(sent) == 0) {
      auto ready = stream.wait(
          true, std::min(deadline, transport.last_activity() + std::chrono::seconds(3)), wake);
      if (auto* error = std::get_if<IpcError>(&ready)) return *error;
    }
  }
  return std::monostate{};
}

core::Result<Message, IpcError> receive_message(IpcStream& stream, MessageTransport& transport,
                                                std::chrono::steady_clock::time_point deadline,
                                                std::stop_token wake) {
  bool started = false;
  for (;;) {
    if (wake.stop_requested())
      return IpcError{IpcErrorCode::ConnectionLost, "IPC wait interrupted"};
    if (std::chrono::steady_clock::now() >= deadline)
      return IpcError{IpcErrorCode::Timeout, "IPC receive timed out"};
    auto received = transport.receive(stream);
    if (auto* error = std::get_if<IpcError>(&received)) return *error;
    if (auto& message = std::get<std::optional<Message>>(received); message)
      return std::move(*message);
    auto wait_deadline = deadline;
    if (transport.receiving()) {
      if (!started) {
        started = true;
        deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(30));
      }
      wait_deadline = std::min(deadline, transport.last_activity() + std::chrono::seconds(3));
    }
    auto ready = stream.wait(false, wait_deadline, wake);
    if (auto* error = std::get_if<IpcError>(&ready)) return *error;
  }
}

core::Result<Message, IpcError> exchange_message(IpcStream& stream, const Message& request,
                                                 std::chrono::steady_clock::time_point deadline) {
  auto sent = send_message(stream, request, deadline);
  if (auto* error = std::get_if<IpcError>(&sent)) return *error;
  MessageTransport transport;
  return receive_message(stream, transport, deadline);
}

} // namespace folio::application::internal
