#include "application/internal/application_codec.hpp"
#include <gtest/gtest.h>

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;

template<class T>
void check_roundtrip(const T& value) {
  auto encoded = ipc::codec::encode(value);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  auto decoded = ipc::codec::decode<T>(std::get<std::string>(encoded));
  ASSERT_TRUE(std::holds_alternative<T>(decoded));
  auto reencoded = ipc::codec::encode(std::get<T>(decoded));
  ASSERT_TRUE(std::holds_alternative<std::string>(reencoded));
  EXPECT_EQ(std::get<std::string>(reencoded), std::get<std::string>(encoded));
}

app::MutationFailure failure(app::AuthoritativeChangeStatus status) {
  return {{app::ErrorCategory::FilesystemFailure, app::ErrorReason::IoFailure,
           "An authoritative step failed.", {app::ErrorAction::Dismiss}}, status,
          {core::Id::generate(), app::DirectoryRef{core::RecordType::Memo},
           app::DirectoryRef{core::Id::generate()},
           {{app::MutationStep::CreateDestination, app::StepObservation::DurabilityConfirmed},
            {app::MutationStep::RemoveSource, app::StepObservation::NotCompleted}}}};
}

TEST(MutationCodec, ApprovedComponentsRoundTripEveryEnumAndStructuredEvidence) {
  check_roundtrip(app::MutationId::generate());
  for (auto status : {app::AuthoritativeChangeStatus::NotApplied, app::AuthoritativeChangeStatus::Applied,
                      app::AuthoritativeChangeStatus::PartiallyApplied, app::AuthoritativeChangeStatus::Unknown})
    check_roundtrip(failure(status));
  for (auto step : {app::MutationStep::WriteEntry, app::MutationStep::CreateDestination, app::MutationStep::RemoveSource})
    for (auto observation : {app::StepObservation::NotAttempted, app::StepObservation::NotCompleted,
                            app::StepObservation::Published, app::StepObservation::DurabilityConfirmed,
                            app::StepObservation::Unknown})
      check_roundtrip(app::MutationStepEvidence{step, observation});
  check_roundtrip(app::MutationEvidence{core::Id::generate(), {}, {}, {}});
}

TEST(MutationCodec, ConfirmedEnvelopesPreserveSuccessFailureAndValidateIdentity) {
  const auto id = app::MutationId::generate();
  // Existing Id is a codec test value, not an approved Create success contract.
  for (const auto& result : {core::Result<core::Id, app::MutationFailure>{core::Id::generate()},
                            core::Result<core::Id, app::MutationFailure>{failure(app::AuthoritativeChangeStatus::Unknown)}}) {
    const app::MutationResponse<core::Id> value{id, result};
    auto encoded = ipc::encode_mutation_response(value);
    ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
    const auto& bytes = std::get<std::string>(encoded);
    auto decoded = ipc::decode_mutation_response<core::Id>(bytes, id);
    ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::Id>>(decoded));
    EXPECT_EQ(std::get<app::MutationResponse<core::Id>>(decoded).result.index(), result.index());
    auto mismatch = ipc::decode_mutation_response<core::Id>(bytes, app::MutationId::generate());
    ASSERT_TRUE(std::holds_alternative<ipc::IpcError>(mismatch));
    EXPECT_EQ(std::get<ipc::IpcError>(mismatch).code, ipc::IpcErrorCode::Protocol);
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::decode_mutation_response<core::Id>(bytes + "x", id)));
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::decode_mutation_response<core::Id>(bytes.substr(0, bytes.size()-1), id)));
  }
}

TEST(MutationCodec, MalformedIdsEnumsVariantsFieldsAndCollectionDeclarationsAreRejected) {
  const std::vector<std::string> malformed{
      "", std::string("\xc1", 1), std::string("\x90", 1), std::string("\x91\xc0", 2),
      std::string("\xdd\xff\xff\xff\xff", 5), std::string("\xdb\xff\xff\xff\xff", 5),
      std::string(40, '\x91') + "\xc0"};
  for (const auto& bytes : malformed) {
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationFailure>(bytes)));
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationEvidence>(bytes)));
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationId>(bytes)));
  }
  for (const auto& text : {std::string("not-a-uuid"), std::string("00000000-0000-0000-0000-00000000000z")}) {
    auto bytes = std::get<std::string>(ipc::codec::encode(text));
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationId>(bytes)));
  }
  for (auto bytes : {std::string("\xff",1), std::string("\x04",1), std::string("\xc3",1)})
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::AuthoritativeChangeStatus>(bytes)));
  auto evidence = failure(app::AuthoritativeChangeStatus::Applied).evidence;
  evidence.steps.push_back({static_cast<app::MutationStep>(55), app::StepObservation::Unknown});
  auto bytes = std::get<std::string>(ipc::codec::encode(evidence));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationEvidence>(bytes)));
  evidence.steps.clear();
  evidence.source = app::DirectoryRef{static_cast<core::RecordType>(77)};
  bytes = std::get<std::string>(ipc::codec::encode(evidence));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationEvidence>(bytes)));
  auto response = std::get<std::string>(ipc::encode_mutation_response(app::MutationResponse<core::Id>{app::MutationId::generate(), core::Id::generate()}));
  // Envelope = [id, [result tag, value]]. Replace success tag with invalid tag.
  const auto tag = response.find('\x92', 1);
  ASSERT_NE(tag, std::string::npos);
  response[tag + 1] = '\x7f';
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::decode<app::MutationResponse<core::Id>>(response)));
}
TEST(MutationCodec, NestedFailureFieldsAreStrictlyValidated) {
  auto encoded = std::get<std::string>(ipc::codec::encode(failure(app::AuthoritativeChangeStatus::Unknown)));
  auto reject = [&](auto change) {
    auto unpacked = msgpack::unpack(encoded.data(), encoded.size());
    auto object = unpacked.get();
    change(object);
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, object);
    auto decoded = ipc::codec::decode<app::MutationFailure>({buffer.data(), buffer.size()});
    EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(decoded));
  };
  reject([](auto& o) { o.via.array.size = 2; });
  reject([](auto& o) { o.via.array.ptr[1] = msgpack::object(99); });
  reject([](auto& o) { o.via.array.ptr[1] = msgpack::object(false); });
  reject([](auto& o) { o.via.array.ptr[2].via.array.ptr[0] = msgpack::object(17); });
  reject([](auto& o) { o.via.array.ptr[2].via.array.ptr[1].via.array.ptr[0].via.array.ptr[0] = msgpack::object(99); });
  reject([](auto& o) { o.via.array.ptr[2].via.array.ptr[3].via.array.ptr[0].via.array.ptr[1] = msgpack::object(99); });
  reject([](auto& o) { o.via.array.ptr[0].via.array.ptr[3] = msgpack::object(false); });
}
}  // namespace
