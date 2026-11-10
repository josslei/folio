#include <folio/ipc/message_transport.hpp>
#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>
#include "operations/mutation_orchestrator.hpp"
#include "operations/access_orchestrator.hpp"

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
} // namespace

TEST(IpcResourceLimits, OwnershipAcquisitionIsIndependentOfOversizedLoad) {
  namespace core = folio::core;
  namespace app = folio::application;
  namespace storage = core::storage;
  namespace fs = std::filesystem;
  const auto base = fs::weakly_canonical(fs::temp_directory_path()) /
                    ("fedit-limit-" + core::Id::generate().to_string());
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{base};
  storage::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state", base / "cache",
                               base / "logs", std::nullopt}};
  fs::create_directories(paths.normal_root);
  storage::MetadataCatalog catalog;
  storage::NormalRepository repository{paths.normal_root};
  storage::CatalogCoordinator coordinator{paths, catalog};
  ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(coordinator.startup_load()));
  core::TimestampContext time{core::Timestamp{}, "UTC", {}};
  core::Memo record{"Oversized", std::string(ipc::max_application_bytes, 'x'), time};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(
      record.snapshot(), coordinator.memo_creation_callback(), coordinator.creation_check())));
  ipc::MutationOrchestrator mutations(catalog, repository, coordinator);
  auto first = std::make_shared<ipc::ConnectionSession>();
  auto second = std::make_shared<ipc::ConnectionSession>();
  auto acquired = mutations.handle(ipc::AcquireEditRequest{record.id()}, first);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(acquired));
  auto loaded = ipc::AccessOrchestrator{catalog, repository, coordinator}.handle(
      ipc::LoadMemoRequest{record.id()});
  ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
  EXPECT_TRUE(std::holds_alternative<ipc::IpcError>(ipc::codec::encode(loaded)));
  EXPECT_EQ(std::get<app::ApplicationError>(
                mutations.handle(ipc::AcquireEditRequest{record.id()}, second))
                .reason,
            app::ErrorReason::LockConflict);
  auto update =
      mutations.handle(ipc::MutationRequest{app::MutationId::generate(),
                                            app::internal::UpdateMemoRequest{
                                                record.id(), {"title", "body", {}}, time}},
                       second);
  const auto& failure =
      std::get<app::MutationFailure>(std::get<ipc::NormalMutationResponse>(update).result);
  EXPECT_EQ(failure.error.reason, app::ErrorReason::LockConflict);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::ReleaseEditRequest{record.id()}, first)));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      mutations.handle(ipc::AcquireEditRequest{record.id()}, second)));
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::NotApplied);
}
