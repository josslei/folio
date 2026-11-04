#include "application/internal/message_transport.hpp"

#include <algorithm>
#include <array>
#include <thread>

namespace folio::application::internal {

core::Result<std::monostate, IpcError> MessageTransport::queue(const Message& message) {
  if (sending())
    return IpcError{IpcErrorCode::Protocol, "A message is already being sent"};
  if (message.payload.size() > max_application_bytes)
    return IpcError{IpcErrorCode::Protocol, "Message exceeds resource limit"};
  output_ = encode_message(message);
  sent_ = 0;
  last_activity_ = std::chrono::steady_clock::now();
  return std::monostate{};
}

core::Result<std::size_t, IpcError> MessageTransport::flush(IpcStream& stream) {
  if (!sending()) return std::size_t{0};
  // A scheduling budget, not a protocol frame boundary. Yield between OS writes
  // so one large response cannot monopolize the Backend's connection loop.
  const auto size = std::min(output_.size() - sent_, std::size_t{65536});
  auto written = stream.write(std::as_bytes(std::span(output_.data(), output_.size())).subspan(sent_, size));
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
  auto decoded = decode_message(input_);
  if (auto* error = std::get_if<IpcError>(&decoded)) return *error;
  auto& message = std::get<std::optional<Message>>(decoded);
  if (message) {
    if (!input_.empty())
      return IpcError{IpcErrorCode::Protocol, "Unexpected trailing message bytes"};
    std::string{}.swap(input_);
  }
  return decoded;
}

core::Result<Message, IpcError> exchange_message(
    IpcStream& stream, const Message& request,
    std::chrono::steady_clock::time_point deadline) {
  MessageTransport transport;
  auto queued = transport.queue(request);
  if (auto* error = std::get_if<IpcError>(&queued)) return *error;
  while (std::chrono::steady_clock::now() < deadline) {
    if (transport.sending()) {
      auto sent = transport.flush(stream);
      if (auto* error = std::get_if<IpcError>(&sent)) return *error;
    } else {
      auto received = transport.receive(stream);
      if (auto* error = std::get_if<IpcError>(&received)) return *error;
      if (auto& message = std::get<std::optional<Message>>(received); message)
        return std::move(*message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return IpcError{IpcErrorCode::Timeout, "Backend message exchange timed out"};
}

}  // namespace folio::application::internal
