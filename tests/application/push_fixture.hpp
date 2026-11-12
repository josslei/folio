#pragma once
#include "backend_connection_handle.hpp"
#include <folio/ipc/application_codec.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <folio/ipc/message_transport.hpp>

namespace folio::test {
namespace ipc = application::internal;
/// Test-only time bound: cancel the production indefinite wait if no event arrives.
inline bool wait_push_notifications(application::BackendConnectionHandle& connection,
                                    std::chrono::steady_clock::time_point deadline) {
  std::stop_source stop;
  std::mutex timer_mutex;
  std::condition_variable_any timer_changed;
  std::jthread timer([&](std::stop_token timer_stop) {
    std::unique_lock lock(timer_mutex);
    timer_changed.wait_until(lock, timer_stop, deadline, [] { return false; });
    if (!timer_stop.stop_requested()) stop.request_stop();
  });
  return connection.wait_push_notifications(stop.get_token());
}
/// Raw-protocol test peer: ACK Push without depending on Frontend private classes.
class PushPeer {
public:
  explicit PushPeer(ipc::IpcStream stream)
      : worker_([this, stream = std::move(stream)](std::stop_token stop) mutable {
          while (!stop.stop_requested()) {
            ipc::MessageTransport transport;
            auto received = ipc::receive_message(
                stream, transport, std::chrono::steady_clock::time_point::max(), stop);
            if (!std::holds_alternative<ipc::Message>(received)) return;
            auto message = std::get<ipc::Message>(std::move(received));
            if (message.kind != ipc::MessageKind::Push) return;
            auto decoded =
                ipc::codec::decode<ipc::PushEnvelope>(ipc::payload_text(message.payload));
            if (!std::holds_alternative<ipc::PushEnvelope>(decoded)) return;
            auto event = std::get<ipc::PushEnvelope>(std::move(decoded));
            auto ack = ipc::codec::encode(ipc::PushReceipt{event.sequence});
            if (!std::holds_alternative<std::string>(ack)) return;
            auto sent = ipc::send_message(
                stream, {ipc::MessageKind::PushAck, ipc::payload_bytes(std::get<std::string>(ack))},
                std::chrono::steady_clock::now() + std::chrono::seconds(3), stop);
            if (!std::holds_alternative<std::monostate>(sent)) return;
            std::lock_guard lock(notifications_mutex_);
            notifications_.push_back(std::move(event.notification));
            changed_.notify_all();
          }
        }) {}
  bool wait(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(notifications_mutex_);
    return changed_.wait_until(lock, deadline, [&] { return !notifications_.empty(); });
  }
  std::vector<application::PushNotification> take() {
    std::lock_guard lock(notifications_mutex_);
    std::vector<application::PushNotification> result;
    result.swap(notifications_);
    return result;
  }

private:
  /// Protects received notifications; worker is destroyed before these members.
  std::mutex notifications_mutex_;
  std::condition_variable changed_;
  std::vector<application::PushNotification> notifications_;
  std::jthread worker_;
};
inline std::unique_ptr<PushPeer> pair_push(const ipc::ConnectionSettings& settings,
                                           const ipc::Message& ready) {
  auto connection = ipc::IpcStream::connect(settings.endpoint);
  if (!std::holds_alternative<ipc::IpcStream>(connection))
    throw std::runtime_error("Push connect failed");
  auto push = std::get<ipc::IpcStream>(std::move(connection));
  auto paired = ipc::exchange_message(
      push,
      {ipc::MessageKind::Hello,
       ipc::payload_bytes(std::get<std::string>(ipc::codec::encode(ipc::HelloPayload{
           ipc::PushHello{*folio::core::Id::parse(ipc::payload_text(ready.payload))}})))},
      std::chrono::steady_clock::now() + std::chrono::seconds(3));
  if (!std::holds_alternative<ipc::Message>(paired) ||
      std::get<ipc::Message>(paired).kind != ipc::MessageKind::Ready)
    throw std::runtime_error("Push pairing failed");
  return std::make_unique<PushPeer>(std::move(push));
}
} // namespace folio::test
