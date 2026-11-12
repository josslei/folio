#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;
TEST(PushCodec, HelloRolesRoundTripAndRejectMalformedPayloads) {
  const auto token = core::Id::generate();
  for (const ipc::HelloPayload role :
       {ipc::HelloPayload{ipc::RequestHello{}}, ipc::HelloPayload{ipc::PushHello{token}}}) {
    auto bytes = std::get<std::string>(ipc::codec::encode(role));
    auto decoded = ipc::codec::decode<ipc::HelloPayload>(bytes);
    ASSERT_TRUE(std::holds_alternative<ipc::HelloPayload>(decoded));
    EXPECT_EQ(std::get<ipc::HelloPayload>(decoded).index(), role.index());
    if (const auto* push = std::get_if<ipc::PushHello>(&std::get<ipc::HelloPayload>(decoded)))
      EXPECT_EQ(push->pairing_token, token);
    EXPECT_TRUE(
        std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::HelloPayload>(bytes + "x")));
    bytes.pop_back();
    EXPECT_TRUE(
        std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::HelloPayload>(bytes)));
  }
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::HelloPayload>("")));
}

TEST(PushCodec, SingleNoticeRoundTripWithoutReplicatingContents) {
  const auto id = core::Id::generate();
  ipc::PushEnvelope source{17, {app::RecordChangeNotice{id, true, false}}};
  auto encoded = std::get<std::string>(ipc::codec::encode(source));
  auto decoded = ipc::codec::decode<ipc::PushEnvelope>(encoded);
  ASSERT_TRUE(std::holds_alternative<ipc::PushEnvelope>(decoded));
  const auto& result = std::get<ipc::PushEnvelope>(decoded);
  EXPECT_EQ(result.sequence, 17u);
  EXPECT_EQ(std::get<app::RecordChangeNotice>(result.notification).id, id);
  EXPECT_FALSE(std::get<app::RecordChangeNotice>(result.notification).content);
  EXPECT_TRUE(
      std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::PushEnvelope>(encoded + "x")));
  encoded.pop_back();
  EXPECT_TRUE(
      std::holds_alternative<ipc::IpcError>(ipc::codec::decode<ipc::PushEnvelope>(encoded)));
}
TEST(PushCodec, EachNoticeHasItsOwnEnvelopeAndReceiptIdentity) {
  const auto id = core::Id::generate();
  const std::vector<app::PushNotification> notices{
      app::RecordChangeNotice{id, true, false},
      app::CatalogChangeNotice{app::RepositoryScope{}, true}, app::EditLockChangeNotice{id},
      app::AvailabilityChangeNotice{app::SharedAvailability::Ready}};
  std::uint64_t sequence = 0;
  for (const auto& notice : notices) {
    ipc::PushEnvelope source{++sequence, {notice}};
    auto bytes = std::get<std::string>(ipc::codec::encode(source));
    auto decoded = ipc::codec::decode<ipc::PushEnvelope>(bytes);
    ASSERT_TRUE(std::holds_alternative<ipc::PushEnvelope>(decoded));
    const auto& result = std::get<ipc::PushEnvelope>(decoded);
    EXPECT_EQ(result.notification.index(), notice.index());
    EXPECT_EQ(result.sequence, sequence);
    auto receipt = ipc::codec::decode<ipc::PushReceipt>(
        std::get<std::string>(ipc::codec::encode(ipc::PushReceipt{result.sequence})));
    ASSERT_TRUE(std::holds_alternative<ipc::PushReceipt>(receipt));
    EXPECT_EQ(std::get<ipc::PushReceipt>(receipt).sequence, sequence);
  }
}

TEST(PushCodec, RejectsMalformedIdentifiersEnumsAndVariantTags) {
  const auto id = core::Id::generate();
  auto bytes = std::get<std::string>(ipc::codec::encode(app::RecordChangeNotice{id, true, false}));
  const auto offset = bytes.find(id.to_string());
  ASSERT_NE(offset, std::string::npos);
  bytes[offset] = 'z';
  EXPECT_TRUE(
      std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::RecordChangeNotice>(bytes)));
  bytes = std::get<std::string>(
      ipc::codec::encode(app::AvailabilityChangeNotice{static_cast<app::SharedAvailability>(99)}));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::codec::decode<app::AvailabilityChangeNotice>(bytes)));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::codec::decode<app::PushNotification>(std::string("\x92\x63\x90", 3))));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(
      ipc::codec::decode<ipc::PushReceipt>(std::string("\x91\xff", 2))));
}
} // namespace
