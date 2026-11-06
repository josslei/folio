#include <folio/ipc/message_transport.hpp>
#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>

namespace {
namespace ipc = folio::application::internal;
TEST(IpcResourceLimits, FullMessageBoundaryAndOverflow) {
  ipc::Message message{ipc::MessageKind::Request,
                       std::vector<std::byte>(ipc::max_application_bytes, std::byte{'x'})};
  auto bytes = ipc::encode_message(message);
  message.payload.clear();
  message.payload.shrink_to_fit();
  EXPECT_EQ(bytes.size(), ipc::max_message_bytes + 4);
  auto decoded = ipc::decode_message(bytes);
  ASSERT_TRUE(std::holds_alternative<std::optional<ipc::Message>>(decoded));
  auto& value = std::get<std::optional<ipc::Message>>(decoded);
  ASSERT_TRUE(value);
  EXPECT_EQ(value->payload.size(), ipc::max_application_bytes);
  value.reset();
  bytes = std::string("\x04\x00\x00\x01", 4);
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::decode_message(bytes)));
}

TEST(IpcResourceLimits, LogicalCodecRejectsOversizedInputAndOutput) {
  const std::string oversized(ipc::max_application_bytes + 1, 'x');
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::encode(oversized)));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<std::string>(oversized)));
}
}  // namespace
