#include <folio/application/backend_connection_handle.hpp>
#include "application/internal/process.hpp"
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
  fs::path base = fs::path("/private/tmp") / ("fread-" + core::Id::generate().to_string().substr(0, 8));
#else
  fs::path base = fs::temp_directory_path() / ("fread-" + core::Id::generate().to_string().substr(0, 8));
#endif
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", {}};
  storage::StoragePaths paths{base / "normal", base / "vault",
      {base / "config", base / "data", base / "state", base / "cache", base / "logs", base / "runtime"}};
  storage::NormalRepository repository{paths.normal_root};
  core::Memo memo{"Long memo", std::string(80 * 1024, 'x'), time};
  std::uint64_t pid = 0;
  fs::path result_file = base / "exited";
  template<class F> bool until(F predicate) {
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
    if (pid && !until([&]{return fs::exists(result_file);})) {
#ifndef _WIN32
      kill(static_cast<pid_t>(pid), SIGKILL);
#else
      auto handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
      if (handle) { TerminateProcess(handle, 9); WaitForSingleObject(handle, 5000); CloseHandle(handle); }
#endif
      ADD_FAILURE() << "Backend failed orderly shutdown";
    }
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  app::BackendConnectionHandle connect() {
    auto connection = app::connect_or_start_backend(
        std::get<ipc::ConnectionSettings>(ipc::connection_settings(paths)), [&] {
          auto launched = ipc::launch_process(FOLIO_BACKEND_CHILD, {"backend", base, result_file});
          if (auto* process = std::get_if<app::LaunchedProcess>(&launched)) pid = process->id;
          return launched;
        }, 8s);
    if (!std::holds_alternative<app::BackendConnectionHandle>(connection))
      throw std::runtime_error(std::get<ipc::IpcError>(connection).message);
    return std::get<app::BackendConnectionHandle>(std::move(connection));
  }
};

TEST_F(MetadataAccessIntegration, OneBrowseOperationHandlesRootsAndUserDirectories) {
  core::Directory directory{core::DirectoryKind::Memo, "Projects"};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_directory(
      directory.kind(), directory.snapshot())));
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
  EXPECT_TRUE(std::get<app::ApplicationResponse<app::BrowseResult>>(logs).value.directories.empty());
  auto missing = connection.browse(core::Id::generate());
  ASSERT_TRUE(std::holds_alternative<app::ApplicationError>(missing));
  EXPECT_EQ(std::get<app::ApplicationError>(missing).reason, app::ErrorReason::NotFound);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}

TEST_F(MetadataAccessIntegration, LargeContentAndMetadataResponsesAllowTwoPeersAndSubsequentPing) {
  // A large metadata string exercises a frame beyond the 64KiB lifecycle limit.
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
    ASSERT_TRUE(std::holds_alternative<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(metadata));
    const auto& records = std::get<app::ApplicationResponse<std::vector<core::RecordMetadata>>>(metadata).value;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(std::get<core::MemoMetadata>(records[0]).title, changed.title);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(second.ping()));
    EXPECT_FALSE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  }
  ASSERT_TRUE(until([&]{return fs::exists(result_file);}));
  EXPECT_TRUE(fs::exists(paths.application.cache / "index/normal.msgpack"));
  EXPECT_FALSE(fs::exists(paths.application.state / "session.marker"));
}

TEST_F(MetadataAccessIntegration, MissingWrongTypeAndStaleErrorsLeaveConnectionUsableWithoutDiscovery) {
  auto connection = connect();
  app::RecordQuery invalid;
  invalid.filters.log_date.from = std::chrono::year{2027}/2/30;
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
}
}  // namespace
