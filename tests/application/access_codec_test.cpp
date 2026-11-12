#include "backend_connection_handle.hpp"
#include <folio/ipc/application_codec.hpp>
#include <gtest/gtest.h>

namespace {
namespace app = folio::application;
namespace wire = app::internal;
namespace core = folio::core;
core::TimestampContext timestamp() {
  return {core::Timestamp{std::chrono::milliseconds{1800000000123}}, "Asia/Tokyo", "location"};
}
core::DateContext date() {
  return {std::chrono::year{2027} / 2 / 3, "UTC", std::nullopt};
}

template <class T, class Encode, class Decode>
void roundtrip(const T& value, Encode encode, Decode decode) {
  auto encoded = encode(value);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  auto decoded = decode(std::get<std::string>(encoded));
  ASSERT_TRUE(std::holds_alternative<T>(decoded));
  EXPECT_EQ(std::get<T>(decoded).index(), value.index());
  auto reencoded = encode(std::get<T>(decoded));
  ASSERT_TRUE(std::holds_alternative<std::string>(reencoded));
  EXPECT_EQ(std::get<std::string>(reencoded), std::get<std::string>(encoded));
}

TEST(AccessCodec, EveryRequestAlternativeAndCompleteFiltersRoundtrip) {
  const auto id = core::Id::generate();
  app::RecordQuery q;
  q.type = core::RecordType::Todo;
  q.scope = app::DirectoryScope{id};
  q.order = {app::MetadataSort::DueDate, true};
  q.filters.privacy = core::Privacy::Normal;
  q.filters.title_contains = std::string("abc\0def", 7);
  q.filters.tag = core::Tag{"tag"};
  q.filters.created = {timestamp().timestamp, timestamp().timestamp + std::chrono::hours{1}};
  q.filters.modified = {std::nullopt, timestamp().timestamp};
  q.filters.log_date = {date().date, date().date};
  q.filters.todo_status = core::TodoStatus::Completed;
  q.filters.todo_priority = core::TodoPriority::High;
  q.filters.has_due_date = false;
  q.filters.due_kind = app::DueKind::Date;
  q.filters.due_from = date();
  q.filters.due_through = timestamp();
  const std::vector<wire::OperationRequest> requests{
      wire::BrowseRequest{core::RecordType::Memo, {app::MetadataSort::Title, true}},
      wire::BrowseRequest{id, {}},
      wire::QueryRecordsRequest{q},
      wire::ListDirectoriesRequest{core::DirectoryKind::Todo},
      wire::LoadLogRequest{id},
      wire::LoadMemoRequest{id},
      wire::LoadTodoRequest{id}};
  for (const auto& request : requests)
    roundtrip(request, wire::codec::encode<wire::OperationRequest>,
              wire::codec::decode<wire::OperationRequest>);
  for (const auto& scope :
       {app::RecordScope{app::RootScope{}}, app::RecordScope{app::AllScope{}}}) {
    q.scope = scope;
    roundtrip(wire::OperationRequest{wire::QueryRecordsRequest{q}},
              wire::codec::encode<wire::OperationRequest>,
              wire::codec::decode<wire::OperationRequest>);
  }
}

TEST(AccessCodec, MetadataWarningsErrorsAndFullSnapshotsPreserveEveryField) {
  const auto id = core::Id::generate();
  const auto directory_id = core::Id::generate();
  const std::string bytes("body\0\xff\xfe", 7);
  const std::vector<app::ApplicationWarning> warnings{
      {app::WarningReason::CatalogInconsistent,
       "warning",
       {app::ErrorAction::Dismiss, app::ErrorAction::ReloadView}}};
  std::vector<core::RecordMetadata> metadata{
      core::LogMetadata{
          id, core::Privacy::Normal, timestamp(), timestamp(), "log", {{"tag"}}, date()},
      core::MemoMetadata{
          id, core::Privacy::Normal, timestamp(), timestamp(), "memo", {{"tag"}}, directory_id},
      core::TodoMetadata{id, core::Privacy::Normal, timestamp(), timestamp(), "todo",
                         core::TodoStatus::Completed, core::TodoPriority::High,
                         core::DueDate{timestamp()}, directory_id}};
  const std::vector<core::DirectoryMetadata> directories{
      {directory_id, core::DirectoryKind::Memo, "name"}};
  const std::vector<wire::OperationResponse> responses{
      app::Annotated<app::BrowseResult>{{metadata, directories}, warnings},
      app::Annotated<std::vector<core::RecordMetadata>>{metadata, warnings},
      app::Annotated<std::vector<core::DirectoryMetadata>>{directories, warnings},
      app::Annotated<core::Log::Snapshot>{
          core::Log::Snapshot{
              id, timestamp(), timestamp(), "log", bytes, date(), {{"tag"}}, core::Privacy::Normal},
          warnings},
      app::Annotated<core::Memo::Snapshot>{core::Memo::Snapshot{id,
                                                                timestamp(),
                                                                timestamp(),
                                                                "memo",
                                                                bytes,
                                                                {{"tag"}},
                                                                directory_id,
                                                                core::Privacy::Normal},
                                           warnings},
      app::Annotated<core::Todo::Snapshot>{core::Todo::Snapshot{id,
                                                                timestamp(),
                                                                timestamp(),
                                                                "todo",
                                                                bytes,
                                                                core::TodoStatus::Completed,
                                                                core::TodoPriority::High,
                                                                core::DueDate{date()},
                                                                directory_id,
                                                                core::Privacy::Normal,
                                                                {{bytes, true}}},
                                           warnings},
      app::ApplicationError{app::ErrorCategory::TargetUnavailable,
                            app::ErrorReason::StaleLocation,
                            "message",
                            {app::ErrorAction::Retry, app::ErrorAction::ReloadView,
                             app::ErrorAction::Reconnect, app::ErrorAction::ResolveExternally,
                             app::ErrorAction::Dismiss}}};
  for (const auto& response : responses)
    roundtrip(response, wire::codec::encode<wire::OperationResponse>,
              wire::codec::decode<wire::OperationResponse>);
}

TEST(AccessCodec, MalformedTruncatedTrailingDeepAndOversizedDeclarationsAreRejected) {
  const auto good_result =
      wire::codec::encode<wire::OperationRequest>(wire::LoadMemoRequest{core::Id::generate()});
  ASSERT_TRUE(std::holds_alternative<std::string>(good_result));
  const auto good = std::get<std::string>(good_result);
  std::vector<std::string> malformed{"",
                                     std::string("\xc1", 1),
                                     std::string("\x92\xff\x90", 3),
                                     std::string("\xdd\xff\xff\xff\xff", 5),
                                     good.substr(0, good.size() - 1),
                                     good + std::string("\xc0", 1)};
  malformed.push_back(std::string(40, static_cast<char>(0x91)) + std::string("\xc0", 1));
  for (const auto& bytes : malformed) {
    auto request = wire::codec::decode<wire::OperationRequest>(bytes);
    EXPECT_TRUE(std::holds_alternative<wire::IpcError>(request));
    auto response = wire::codec::decode<wire::OperationResponse>(bytes);
    EXPECT_TRUE(std::holds_alternative<wire::IpcError>(response));
  }
}

TEST(AccessCodec, TransportDiagnosticsNeverExposeArbitraryPeerText) {
  const auto translated =
      wire::operation_connection_error({wire::IpcErrorCode::ConnectionLost, "sensitive peer text"});
  EXPECT_EQ(translated.category, app::ErrorCategory::ConnectionFailure);
  EXPECT_EQ(translated.reason, app::ErrorReason::ConnectionLost);
  EXPECT_EQ(translated.message.find("sensitive"), std::string::npos);
}
} // namespace
