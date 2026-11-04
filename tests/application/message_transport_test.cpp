#include "application/internal/message_transport.hpp"
#include "application/internal/repository_ownership.hpp"
#include <gtest/gtest.h>

#include <thread>

namespace {
namespace ipc = folio::application::internal;
namespace core = folio::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

TEST(MessageProtocol, CompleteMessagesHaveNo64KiBBoundary) {
  for (const auto size : {0u, 1u, 65532u, 65533u, 150000u}) {
    auto bytes = ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes(std::string(size, '\xff'))});
    const auto split = bytes.size() - 1;
    auto prefix = bytes.substr(0, split);
    auto incomplete = ipc::decode_message(prefix);
    ASSERT_TRUE(std::holds_alternative<std::optional<ipc::Message>>(incomplete));
    EXPECT_FALSE(std::get<std::optional<ipc::Message>>(incomplete));
    prefix += bytes.substr(split);
    auto decoded = ipc::decode_message(prefix);
    ASSERT_TRUE(std::holds_alternative<std::optional<ipc::Message>>(decoded));
    auto message = std::get<std::optional<ipc::Message>>(std::move(decoded));
    ASSERT_TRUE(message);
    EXPECT_EQ(message->kind, ipc::MessageKind::Request);
    EXPECT_EQ(message->payload.size(), size);
    EXPECT_TRUE(prefix.empty());
  }
}

TEST(MessageProtocol, InvalidHeadersAreRejectedWithoutPayloadAllocation) {
  for (const auto& bytes : {std::string("\x04\x00\x00\x01", 4), std::string("\0\0\0\3", 4),
                           ipc::encode_message({ipc::MessageKind::Request, {}, 2}),
                           ipc::encode_message({static_cast<ipc::MessageKind>(8), {}}),
                           ipc::encode_message({static_cast<ipc::MessageKind>(9), {}}),
                           ipc::encode_message({static_cast<ipc::MessageKind>(10), {}})}) {
    auto input = bytes;
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::decode_message(input)));
  }
}

class MessageTransport : public testing::Test {
 protected:
#ifdef __APPLE__
  fs::path base = fs::path("/private/tmp") / ("fmsg-" + core::Id::generate().to_string().substr(0, 8));
#else
  fs::path base = fs::temp_directory_path() / ("fmsg-" + core::Id::generate().to_string().substr(0, 8));
#endif
  folio::config::StoragePaths paths{base / "normal", base / "vault",
      {base / "config", base / "data", base / "state", base / "cache", base / "logs", base / "runtime"}};
  std::optional<ipc::RepositoryOwnership> owner;
  ipc::IpcStream sender, receiver;
  template<class F> bool until(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      if (predicate()) return true;
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
  void SetUp() override {
    auto acquired = ipc::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<ipc::RepositoryOwnership>(acquired));
    owner.emplace(std::get<ipc::RepositoryOwnership>(std::move(acquired)));
    auto settings = std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths));
    auto connected = ipc::IpcStream::connect(settings.endpoint);
    ASSERT_TRUE(std::holds_alternative<ipc::IpcStream>(connected));
    sender = std::get<ipc::IpcStream>(std::move(connected));
    ASSERT_TRUE(until([&] {
      auto accepted = owner->accept();
      if (auto* stream = std::get_if<std::optional<ipc::IpcStream>>(&accepted); stream && *stream)
        receiver = std::move(**stream);
      return receiver.connected();
    }));
  }
  void TearDown() override {
    sender = ipc::IpcStream{};
    receiver = ipc::IpcStream{};
    owner.reset();
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  void write_bytes(std::string_view bytes) {
    std::size_t offset = 0;
    ASSERT_TRUE(until([&] {
      auto sent = sender.write(std::as_bytes(std::span(bytes.data(), bytes.size())).subspan(offset));
      if (auto* count = std::get_if<std::size_t>(&sent)) offset += *count;
      return offset == bytes.size();
    }));
  }
};

TEST_F(MessageTransport, LargeMessageUsesPartialTransfersAndAllowsNextExchange) {
  ipc::MessageTransport output, input;
  for (const auto size : {150000u, 1u}) {
    const auto payload = ipc::payload_bytes(std::string(size, 'x'));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(output.queue({ipc::MessageKind::Request, payload})));
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(output.queue({ipc::MessageKind::Ping, {}})));
    std::optional<ipc::Message> message;
    ASSERT_TRUE(until([&] {
      auto sent = output.flush(sender);
      if (std::holds_alternative<ipc::IpcError>(sent)) return false;
      auto received = input.receive(receiver);
      if (auto* value = std::get_if<std::optional<ipc::Message>>(&received); value && *value)
        message = std::move(**value);
      return message.has_value();
    }));
    EXPECT_EQ(message->payload, payload);
    EXPECT_FALSE(output.sending());
    EXPECT_FALSE(input.receiving());
  }
}

TEST_F(MessageTransport, IncompleteMessageIsNotDeliveredAndEofIsExplicit) {
  ipc::MessageTransport input;
  auto bytes = ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes("payload")});
  write_bytes(std::string_view(bytes).substr(0, bytes.size() - 1));
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto result = input.receive(receiver);
    ASSERT_TRUE(std::holds_alternative<std::optional<ipc::Message>>(result));
    EXPECT_FALSE(std::get<std::optional<ipc::Message>>(result));
  }
  EXPECT_TRUE(input.receiving());
  sender = ipc::IpcStream{};
  std::optional<ipc::IpcError> error;
  ASSERT_TRUE(until([&] {
    auto result = input.receive(receiver);
    if (auto* problem = std::get_if<ipc::IpcError>(&result)) error = *problem;
    return error.has_value();
  }));
  EXPECT_EQ(error->code, ipc::IpcErrorCode::ConnectionLost);
}

TEST_F(MessageTransport, TrailingMessageBytesAreProtocolFailure) {
  ipc::MessageTransport input;
  write_bytes(ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes("x")}) +
              ipc::encode_message({ipc::MessageKind::Ping, {}}));
  std::optional<ipc::IpcError> error;
  ASSERT_TRUE(until([&] {
    auto result = input.receive(receiver);
    if (auto* problem = std::get_if<ipc::IpcError>(&result)) error = *problem;
    return error.has_value();
  }));
  EXPECT_EQ(error->code, ipc::IpcErrorCode::Protocol);
}
}  // namespace
