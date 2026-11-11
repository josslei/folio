#include "backend_connection_handle.hpp"
#include "process.hpp"
#include <folio/ipc/application_codec.hpp>
#include <folio/ipc/message_transport.hpp>
#include <algorithm>
#include <array>
#include "storage/normal_repository.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <signal.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace {
namespace app = folio::application;
namespace ipc = app::internal;
namespace core = folio::core;
namespace storage = core::storage;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

class MetadataAccessIntegration : public testing::Test {
protected:
#ifdef __APPLE__
  fs::path base =
      fs::path("/private/tmp") / ("fread-" + core::Id::generate().to_string().substr(0, 8));
#else
  fs::path base =
      fs::temp_directory_path() / ("fread-" + core::Id::generate().to_string().substr(0, 8));
#endif
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", {}};
  storage::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state", base / "cache",
                               base / "logs", base / "runtime"}};
  storage::NormalRepository repository{paths.normal_root};
  core::Memo memo{"Long memo", std::string(80 * 1024, 'x'), time};
  struct Child {
    std::uint64_t pid;
    fs::path result;
  };
  std::vector<Child> children;
  bool all_exited() const {
    return std::all_of(children.begin(), children.end(),
                       [](const Child& child) { return fs::exists(child.result); });
  }
  template <class F> bool until(F predicate) {
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) return true;
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }
  void SetUp() override {
    fs::create_directories(paths.normal_root);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(memo.snapshot())));
  }
  void TearDown() override {
    for (const auto& child : children) {
      if (until([&] { return fs::exists(child.result); })) continue;
#ifndef _WIN32
      kill(static_cast<pid_t>(child.pid), SIGKILL);
#else
      auto handle =
          OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(child.pid));
      if (handle) {
        TerminateProcess(handle, 9);
        WaitForSingleObject(handle, 5000);
        CloseHandle(handle);
      }
#endif
      ADD_FAILURE() << "Backend candidate failed orderly shutdown: " << child.pid;
    }
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  bool send_bytes(ipc::IpcStream& stream, std::string_view bytes) {
    std::size_t offset = 0;
    return until([&] {
      auto sent =
          stream.write(std::as_bytes(std::span(bytes.data(), bytes.size())).subspan(offset));
      if (std::holds_alternative<ipc::IpcError>(sent)) return false;
      offset += std::get<std::size_t>(sent);
      return offset == bytes.size();
    });
  }
  bool send(ipc::IpcStream& stream, const ipc::Message& message) {
    return send_bytes(stream, ipc::encode_message(message));
  }
  std::optional<ipc::Message> receive(ipc::IpcStream& stream) {
    ipc::MessageTransport transport;
    auto received = ipc::receive_message(stream, transport, std::chrono::steady_clock::now() + 8s);
    if (auto* message = std::get_if<ipc::Message>(&received)) return std::move(*message);
    return std::nullopt;
  }
  ipc::IpcStream raw_connection() {
    auto settings = std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths));
    auto connected = ipc::IpcStream::connect(settings.endpoint);
    if (!std::holds_alternative<ipc::IpcStream>(connected))
      throw std::runtime_error("Raw connection failed");
    auto stream = std::get<ipc::IpcStream>(std::move(connected));
    if (!send(stream, {ipc::MessageKind::Hello, {}})) throw std::runtime_error("Hello failed");
    auto ready = receive(stream);
    if (!ready || ready->kind != ipc::MessageKind::Ready) throw std::runtime_error("Ready failed");
    return stream;
  }
  app::BackendConnectionHandle connect() {
    auto connection = app::connect_or_start_backend(
        std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths)),
        [&] {
          // Startup can launch multiple candidates. Each must report separately:
          // an AlreadyOwned loser exiting does not mean the active Backend ended.
          const auto result_file = base / ("exited-" + std::to_string(children.size()));
          auto launched = ipc::launch_process(FOLIO_BACKEND_CHILD, {"backend", base, result_file});
          if (auto* process = std::get_if<app::LaunchedProcess>(&launched))
            children.push_back({process->id, result_file});
          return launched;
        },
        8s);
    if (!std::holds_alternative<app::BackendConnectionHandle>(connection))
      throw std::runtime_error(std::get<ipc::IpcError>(connection).message);
    return std::get<app::BackendConnectionHandle>(std::move(connection));
  }
};

TEST_F(MetadataAccessIntegration, OneBrowseOperationHandlesRootsAndUserDirectories) {
  core::Directory directory{core::DirectoryKind::Memo, "Projects"};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      repository.create_directory(directory.kind(), directory.snapshot())));
  core::Memo member{"Member", "body", time};
  member.set_directory_id(directory.id(), time);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_memo(member.snapshot())));

  auto connection = connect();
  auto root = connection.browse(core::RecordType::Memo);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(root));
  const auto& root_contents = std::get<app::ApplicationResponse<app::BrowseResult>>(root).value;
  ASSERT_EQ(root_contents.records.size(), 1u);
  ASSERT_EQ(root_contents.directories.size(), 1u);
  EXPECT_EQ(std::get<core::MemoMetadata>(root_contents.records[0]).id, memo.id());
  EXPECT_EQ(root_contents.directories[0].id, directory.id());

  auto nested = connection.browse(directory.id(), {app::MetadataSort::Title, true});
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(nested));
  const auto& contents = std::get<app::ApplicationResponse<app::BrowseResult>>(nested).value;
  ASSERT_EQ(contents.records.size(), 1u);
  EXPECT_EQ(std::get<core::MemoMetadata>(contents.records[0]).id, member.id());
  EXPECT_TRUE(contents.directories.empty());

  auto logs = connection.browse(core::RecordType::Log);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(logs));
  EXPECT_TRUE(
      std::get<app::ApplicationResponse<app::BrowseResult>>(logs).value.directories.empty());
  auto missing = connection.browse(core::Id::generate());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(missing));
  EXPECT_EQ(std::get<app::ApplicationError>(missing).reason, app::ErrorReason::NotFound);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}

TEST_F(MetadataAccessIntegration, LargeContentAndMetadataMessagesAllowTwoPeersAndSubsequentPing) {
  // A large metadata string exercises partial transfers of one complete message.
  // No arbitrary bulk fixture or whole-repository scan is needed at runtime.
  auto changed = memo.snapshot();
  changed.title = std::string(70 * 1024, 'm');
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.update_memo(changed)));
  {
    auto first = connect();
    auto second_result = app::BackendConnectionHandle::connect(
        std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths)));
    ASSERT_TRUE(std::holds_alternative<app::BackendConnectionHandle>(second_result));
    auto& second = std::get<app::BackendConnectionHandle>(second_result);
    auto loaded = first.load_memo(memo.id());
    ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
    EXPECT_EQ(std::get<core::Memo::Snapshot>(loaded).content, memo.content());
    EXPECT_EQ(std::get<core::Memo::Snapshot>(loaded).title, changed.title);
    auto metadata = second.query_records();
    ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(
        metadata));
    const auto& records =
        std::get<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(metadata).value;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(std::get<core::MemoMetadata>(records[0]).title, changed.title);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(second.ping()));
    EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  }
  ASSERT_TRUE(until([&] { return all_exited(); }));
  EXPECT_TRUE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  EXPECT_FALSE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MetadataAccessIntegration,
       MissingWrongTypeAndStaleErrorsLeaveConnectionUsableWithoutDiscovery) {
  auto connection = connect();
  auto other = connect();
  app::RecordQuery invalid;
  invalid.filters.log_date.from = std::chrono::year{2027} / 2 / 30;
  auto rejected = connection.query_records(invalid);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(rejected));
  EXPECT_EQ(std::get<app::ApplicationError>(rejected).category, app::ErrorCategory::InvalidRequest);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  auto wrong = connection.load_todo(memo.id());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(wrong));
  EXPECT_EQ(std::get<app::ApplicationError>(wrong).reason, app::ErrorReason::WrongRecordType);
  auto missing = connection.load_memo(core::Id::generate());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(missing));
  EXPECT_EQ(std::get<app::ApplicationError>(missing).reason, app::ErrorReason::NotFound);
  auto locator = repository.locate_record(core::RecordType::Memo, memo.id());
  ASSERT_TRUE(std::holds_alternative<fs::path>(locator));
  // Malformed external content must not affect the already-cached metadata or a targeted load.
  std::ofstream(paths.normal_root / "memos/unrelated.md") << "malformed external Record";
  EXPECT_TRUE(std::holds_alternative<core::Memo::Snapshot>(connection.load_memo(memo.id())));
  fs::remove(paths.normal_root / std::get<fs::path>(locator));
  auto stale = connection.load_memo(memo.id());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(stale));
  EXPECT_EQ(std::get<app::ApplicationError>(stale).reason, app::ErrorReason::StaleLocation);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  // Request-specific failures must not be broadcast or terminate other sessions.
  auto browsed = other.browse(core::RecordType::Memo);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(browsed));
  EXPECT_EQ(std::get<app::ApplicationResponse<app::BrowseResult>>(browsed).value.records.size(),
            1u);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(other.ping()));
}
TEST_F(MetadataAccessIntegration, LargeQueryRequestUsesOneMessageAndConnectionRemainsUsable) {
  auto connection = connect();
  app::RecordQuery query;
  query.type = core::RecordType::Memo;
  query.filters.title_contains = std::string(150000, 'z');
  auto result = connection.query_records(query);
  ASSERT_TRUE(
      std::holds_alternative<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(result));
  EXPECT_TRUE(
      std::get<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(result).value.empty());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  EXPECT_TRUE(std::holds_alternative<core::Memo::Snapshot>(connection.load_memo(memo.id())));
  EXPECT_TRUE(
      std::holds_alternative<app::ApplicationError>(connection.load_log(core::Id::generate())));
  EXPECT_TRUE(
      std::holds_alternative<app::ApplicationResponse<std::vector<core::DirectoryMetadata>>>(
          connection.list_directories(core::DirectoryKind::Memo)));
}

TEST_F(MetadataAccessIntegration, PartialRequestWaitsForCompletionWithoutBlockingOtherConnections) {
  auto keeper = connect();
  auto stream = raw_connection();
  const auto request = std::get<std::string>(
      ipc::codec::encode<ipc::OperationRequest>(ipc::LoadMemoRequest{memo.id()}));
  const auto bytes = ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes(request)});
  ASSERT_TRUE(send_bytes(stream, std::string_view(bytes).substr(0, bytes.size() - 1)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(keeper.ping()));
  std::array<std::byte, 8> unexpected{};
  auto pending = stream.read(unexpected);
  ASSERT_TRUE(std::holds_alternative<std::size_t>(pending));
  EXPECT_EQ(std::get<std::size_t>(pending), 0u);
  ASSERT_TRUE(send_bytes(stream, std::string_view(bytes).substr(bytes.size() - 1)));
  auto accepted = receive(stream);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->kind, ipc::MessageKind::Accepted);
  EXPECT_TRUE(accepted->payload.empty());
  auto response = receive(stream);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->kind, ipc::MessageKind::Response);
  auto decoded = ipc::codec::decode<ipc::OperationResponse>(ipc::payload_text(response->payload));
  ASSERT_TRUE(std::holds_alternative<ipc::OperationResponse>(decoded));
  EXPECT_TRUE(
      std::holds_alternative<core::Memo::Snapshot>(std::get<ipc::OperationResponse>(decoded)));
}

TEST_F(MetadataAccessIntegration,
       PrematureDisconnectAndInvalidMessagesDoNotAffectOtherConnections) {
  auto keeper = connect();
  {
    auto stream = raw_connection();
    const auto bytes =
        ipc::encode_message({ipc::MessageKind::Request, ipc::payload_bytes("incomplete")});
    ASSERT_TRUE(send_bytes(stream, std::string_view(bytes).substr(0, bytes.size() - 1)));
    // Closing the stream must not dispatch its incomplete request.
  }
  {
    auto stream = raw_connection();
    ASSERT_TRUE(send(stream, {ipc::MessageKind::Response, {}}));
    auto rejected = receive(stream);
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->kind, ipc::MessageKind::Reject);
  }
  EXPECT_TRUE(std::holds_alternative<std::monostate>(keeper.ping()));
  EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
}

TEST_F(MetadataAccessIntegration, MalformedCompleteRequestsKeepApplicationErrorSemantics) {
  auto keeper = connect();
  for (const auto& payload : {std::string{}, std::string("malformed")}) {
    auto stream = raw_connection();
    ASSERT_TRUE(send(stream, {ipc::MessageKind::Request, ipc::payload_bytes(payload)}));
    auto message = receive(stream);
    ASSERT_TRUE(message);
    ASSERT_EQ(message->kind, ipc::MessageKind::Rejected);
    auto decoded = ipc::codec::decode<app::ApplicationError>(ipc::payload_text(message->payload));
    ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(decoded));
    EXPECT_EQ(std::get<app::ApplicationError>(decoded).reason, app::ErrorReason::InvalidQuery);
    ASSERT_TRUE(send(stream, {ipc::MessageKind::Ping, {}}));
    auto pong = receive(stream);
    ASSERT_TRUE(pong);
    EXPECT_EQ(pong->kind, ipc::MessageKind::Pong);
  }
}
} // namespace

TEST_F(MetadataAccessIntegration,
       MutationOrchestratorAndEditLocksShareExistingConnectionAndBackend) {
  {
    auto first = connect();
    auto second = connect();
    auto dir_result =
        first.create(app::MutationId::generate(), core::DirectoryKind::Memo, "Projects");
    ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::DirectoryMetadata>>(dir_result));
    auto dir_response = std::get<app::MutationResponse<core::DirectoryMetadata>>(dir_result);
    ASSERT_TRUE(std::holds_alternative<core::DirectoryMetadata>(dir_response.result));
    auto directory = std::get<core::DirectoryMetadata>(dir_response.result);
    auto create_id = app::MutationId::generate();
    auto created = first.create(
        create_id, app::MemoEditableFields{"Created", "original body", {{"tag"}}}, time, time, {});
    ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::MemoMetadata>>(created));
    auto response = std::get<app::MutationResponse<core::MemoMetadata>>(created);
    EXPECT_EQ(response.mutation_id, create_id);
    ASSERT_TRUE(std::holds_alternative<core::MemoMetadata>(response.result));
    const auto record = std::get<core::MemoMetadata>(response.result);
    EXPECT_NE(record.id, memo.id());
    auto editing = first.acquire_edit(record.id);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(editing));
    auto editable = first.load_memo(record.id);
    ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(editable));
    EXPECT_EQ(std::get<core::Memo::Snapshot>(editable).content, "original body");
    auto denied = second.move(app::MutationId::generate(), record.id, directory.id, time);
    ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::RecordMetadata>>(denied));
    auto rejected = std::get<app::MutationFailure>(
        std::get<app::MutationResponse<core::RecordMetadata>>(denied).result);
    EXPECT_EQ(rejected.error.reason, app::ErrorReason::LockConflict);
    EXPECT_EQ(rejected.status, app::AuthoritativeChangeStatus::NotApplied);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(second.ping()));
    const core::TimestampContext changed{time.timestamp + 1h, "Asia/Shanghai", "frontend"};
    auto moved = first.move(app::MutationId::generate(), record.id, directory.id, changed);
#ifdef _WIN32
    EXPECT_EQ(std::get<app::MutationFailure>(
                  std::get<app::MutationResponse<core::RecordMetadata>>(moved).result)
                  .status,
              app::AuthoritativeChangeStatus::Unknown);
#else
    ASSERT_TRUE(std::holds_alternative<core::RecordMetadata>(
        std::get<app::MutationResponse<core::RecordMetadata>>(moved).result));
#endif
    auto updated = first.update(
        app::MutationId::generate(), record.id,
        app::MemoEditableFields{"Edited", std::string(100 * 1024, 'e'), {{"edited"}}}, changed);
    ASSERT_TRUE(std::holds_alternative<core::MemoMetadata>(
        std::get<app::MutationResponse<core::MemoMetadata>>(updated).result));
    auto loaded = second.load_memo(record.id);
    ASSERT_TRUE(std::holds_alternative<core::Memo::Snapshot>(loaded));
    auto state = std::get<core::Memo::Snapshot>(loaded);
    EXPECT_EQ(state.directory_id, directory.id);
    EXPECT_EQ(state.content.size(), 100u * 1024u);
    EXPECT_EQ(state.created.timestamp, time.timestamp);
    EXPECT_EQ(state.modified.timezone, changed.timezone);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.release_edit(record.id)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(second.acquire_edit(record.id)));
    // Directory rename uses the same ownership rules, without locking members.
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.acquire_edit(directory.id)));
    auto blocked = second.rename_directory(app::MutationId::generate(), directory.id, "Denied");
    EXPECT_EQ(std::get<app::MutationFailure>(
                  std::get<app::MutationResponse<core::DirectoryMetadata>>(blocked).result)
                  .error.reason,
              app::ErrorReason::LockConflict);
    auto renamed = first.rename_directory(app::MutationId::generate(), directory.id, "Renamed");
    EXPECT_TRUE(std::holds_alternative<core::DirectoryMetadata>(
        std::get<app::MutationResponse<core::DirectoryMetadata>>(renamed).result));
    auto root = second.browse(directory.id);
    ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(root));
    EXPECT_EQ(std::get<app::ApplicationResponse<app::BrowseResult>>(root).value.records.size(), 1u);
  }
  ASSERT_TRUE(until([&] { return all_exited(); }));
  EXPECT_FALSE(fs::exists(paths.application.state / "session.marker"));
  EXPECT_TRUE(fs::exists(paths.application.cache / "index/normal.msgpack"));
}

namespace {
TEST_F(MetadataAccessIntegration, CheckAndRefreshUseExistingConnectionAndKeepBrowseCatalogOnly) {
  auto connection = connect();
  ASSERT_EQ(
      std::get<app::ConsistencyCheckResult>(connection.check_consistency(core::RecordType::Memo)),
      app::ConsistencyCheckResult::NoAnomalyDetected);
  const auto old_path =
      paths.normal_root /
      std::get<fs::path>(repository.locate_record(core::RecordType::Memo, memo.id()));
  fs::rename(old_path, old_path.parent_path() / "renamed.md");
  auto loaded = connection.load_memo(memo.id());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(loaded));
  EXPECT_EQ(std::get<app::ApplicationError>(loaded).reason, app::ErrorReason::StaleLocation);
  auto browse = connection.browse(core::RecordType::Memo);
  ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<app::BrowseResult>>(browse));
  EXPECT_EQ(std::get<app::ApplicationResponse<app::BrowseResult>>(browse).value.records.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.refresh()));
  EXPECT_TRUE(std::holds_alternative<core::Memo::Snapshot>(connection.load_memo(memo.id())));
  std::ofstream(paths.normal_root / "memos" / "extra.txt") << "extra";
  EXPECT_EQ(
      std::get<app::ConsistencyCheckResult>(connection.check_consistency(core::RecordType::Memo)),
      app::ConsistencyCheckResult::NoAnomalyDetected);
  // A second Frontend shares the session observation, even after Refresh and
  // external changes. Repeated navigation must not trigger another disk check.
  auto second = connect();
  EXPECT_EQ(std::get<app::ConsistencyCheckResult>(second.check_consistency(core::RecordType::Memo)),
            app::ConsistencyCheckResult::NoAnomalyDetected);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.refresh(app::DirectoryScanScope{
      app::DirectoryRef{core::RecordType::Memo}, app::RefreshTraversal::Immediate})));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}
} // namespace
