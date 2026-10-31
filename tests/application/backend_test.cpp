#include "application/internal/process.hpp"
#include <folio/application/backend.hpp>
#include <folio/application/backend_connection_handle.hpp>
#include <folio/application/ipc.hpp>
#include "application/internal/repository_ownership.hpp"
#include "storage/catalog_coordinator.hpp"
#include "storage/index_codec.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif
namespace {
namespace fs = std::filesystem;
namespace app = folio::application::internal;
namespace storage = folio::core::storage;
using namespace std::chrono_literals;

void write(const fs::path& p, std::string_view bytes = "") {
  std::ofstream out(p, std::ios::binary);
  out << bytes;
}

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

template <class F>
bool until(F predicate) {
  auto deadline = std::chrono::steady_clock::now() + 8s;
  do {
    if (predicate())
      return true;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void terminate(std::uint64_t id) {
#ifndef _WIN32
  kill(static_cast<pid_t>(id), SIGKILL);
#else
  auto handle =
      OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(id));
  if (handle) {
    TerminateProcess(handle, 9);
    WaitForSingleObject(handle, 5000);
    CloseHandle(handle);
  }
#endif
}

class Backend : public testing::Test {
 protected:
  fs::path base;
  storage::StoragePaths paths;

  struct Child {
    std::uint64_t id;
    fs::path result;
    bool killed = false;
  };

  std::vector<Child> children;

  void SetUp() override {
    base = fs::temp_directory_path() /
           ("fipc-" + folio::core::Id::generate().to_string().substr(0, 8));
    fs::create_directory(base);
    base = fs::canonical(base);
    paths = {base / "normal",
             base / "vault",
             {base / "config", base / "data", base / "state", base / "cache", base / "logs",
              base / "runtime"}};
  }

  void TearDown() override {
    for (auto& child : children) {
      if (!child.killed && !until([&] { return fs::exists(child.result); })) {
        terminate(child.id);
        child.killed = true;
        ADD_FAILURE() << "Backend did not exit: " << child.id;
      }
    }
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }

  auto launch()
      -> folio::core::Result<folio::application::LaunchedProcess, folio::application::ProcessError> {
    auto result_path = base / ("result-" + std::to_string(children.size()));
    auto result =
        app::launch_process(FOLIO_BACKEND_CHILD, {"backend", base, result_path});
    if (auto* process = std::get_if<folio::application::LaunchedProcess>(&result))
      children.push_back({process->id, result_path});
    return result;
  }

  folio::application::BackendConnectionHandle connect() {
    auto result = folio::application::connect_or_start_backend(
        std::get<app::ConnectionSettings>(app::connection_settings(paths)),
        [&] { return launch(); }, 8s);
    if (auto* error = std::get_if<app::IpcError>(&result))
      throw std::runtime_error(error->message);
    return std::get<folio::application::BackendConnectionHandle>(std::move(result));
  }

  fs::path marker() {
    return paths.application.state / "session.marker";
  }

  fs::path index() {
    return paths.application.cache / "index/normal.msgpack";
  }

  void clean_cache() {
    auto owner = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(owner));
    storage::MetadataCatalog catalog;
    storage::CatalogCoordinator coordinator(paths, catalog);
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(
        coordinator.startup_load()));
    ASSERT_TRUE(
        std::holds_alternative<storage::SaveOutcome>(coordinator.finish_session()));
  }
};

TEST_F(Backend, OwnershipStopsEndpointBeforeReleasingLocks) {
  {
    auto acquired = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(acquired));
    auto& ownership = std::get<app::RepositoryOwnership>(acquired);
    auto endpoint = std::get<app::ConnectionSettings>(app::connection_settings(paths)).endpoint;
    auto connected = app::IpcStream::connect(endpoint);
    ASSERT_TRUE(std::holds_alternative<app::IpcStream>(connected));
    ownership.stop_listening();
    ownership.stop_listening();
    EXPECT_TRUE(std::holds_alternative<app::IpcError>(ownership.accept()));
    auto competing = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(competing));
    EXPECT_EQ(std::get<folio::application::ProcessError>(competing).code,
              folio::application::ProcessErrorCode::AlreadyOwned);
    EXPECT_FALSE(fs::exists(marker()));
  }
  auto reacquired = app::RepositoryOwnership::acquire(paths);
  EXPECT_TRUE(std::holds_alternative<app::RepositoryOwnership>(reacquired));
}

TEST_F(Backend, StartsOnDemandReadinessRequiresMarkerAndLastDisconnectSaves) {
  {
    auto connection = connect();
    EXPECT_TRUE(fs::exists(marker()));
    EXPECT_FALSE(fs::exists(index()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
    auto other = folio::application::BackendConnectionHandle::connect(
        std::get<app::ConnectionSettings>(app::connection_settings(paths)));
    ASSERT_TRUE(std::holds_alternative<folio::application::BackendConnectionHandle>(other));
    EXPECT_EQ(children.size(), 1u);
  }
  ASSERT_TRUE(until([&] { return fs::exists(children[0].result); }));
  EXPECT_EQ(read(children[0].result), "0");
  EXPECT_FALSE(fs::exists(marker()));
  EXPECT_TRUE(fs::exists(index()));
}

TEST_F(Backend, IndependentFrontendAndBackendProcessesHaveSeparateSessionLogs) {
  auto keeper = connect();
  const auto control = base / "frontend-control";
  fs::create_directory(control);
  auto launched = app::launch_process(FOLIO_BACKEND_CHILD, {"frontend", base, control});
  ASSERT_TRUE(std::holds_alternative<folio::application::LaunchedProcess>(launched));
  children.push_back({std::get<folio::application::LaunchedProcess>(launched).id, control / "done"});
  ASSERT_TRUE(until([&] { return fs::exists(control / "connected"); }));
  write(control / "disconnect");
  ASSERT_TRUE(until([&] { return fs::exists(control / "done"); }));
  fs::path backend_file, frontend_file;
  for (const auto& entry : fs::directory_iterator(paths.application.logs / "backend"))
    backend_file = entry.path();
  for (const auto& entry : fs::directory_iterator(paths.application.logs / "frontend"))
    frontend_file = entry.path();
  ASSERT_FALSE(backend_file.empty());
  ASSERT_FALSE(frontend_file.empty());
  EXPECT_NE(backend_file.filename(), frontend_file.filename());
  EXPECT_NE(read(frontend_file).find("[frontend]"), std::string::npos);
  EXPECT_EQ(read(frontend_file).find("[backend]"), std::string::npos);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(keeper.ping()));
}

TEST_F(Backend, NewFrontendConfigurationDoesNotReconfigureActiveBackend) {
  fs::create_directory(paths.application.config);
  const auto file = paths.application.config / "folio_config.toml";
  write(file, "[general.normal_storage]\nuse_default_location = false\npath = '" + paths.normal_root.generic_string() +
                  "'\n[general.vault_storage]\nuse_default_location = false\npath = '" + paths.vault_root.generic_string() + "'\n");
  {
    auto first = connect();
    const auto changed_root = base / "changed-normal";
    write(file, "[general.normal_storage]\nuse_default_location = false\npath = '" + changed_root.generic_string() + "'\n");
    auto settings = folio::config::load_configuration(paths.application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(settings));
    auto second = folio::application::connect_or_start_backend(
        std::get<app::ConnectionSettings>(app::connection_settings(
            std::get<folio::config::EffectiveConfig>(settings).paths)),
        [&] { return launch(); }, 8s);
    ASSERT_TRUE(std::holds_alternative<folio::application::BackendConnectionHandle>(second));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        std::get<folio::application::BackendConnectionHandle>(second).ping()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_EQ(children.size(), 1u);
    EXPECT_FALSE(fs::exists(changed_root));
    EXPECT_TRUE(fs::exists(paths.normal_root / ".folio-backend.lock"));
  }
  ASSERT_TRUE(until([&] { return fs::exists(children.front().result); }));
  EXPECT_EQ(read(children.front().result), "0");
}

TEST_F(Backend, OneDisconnectLeavesOtherClientAndReadOnlyIndexUnchanged) {
  clean_cache();
  const auto time = fs::last_write_time(index());
  {
    auto first = connect();
    {
      auto second = connect();
      EXPECT_TRUE(std::holds_alternative<std::monostate>(second.ping()));
    }
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_FALSE(fs::exists(children[0].result));
  }
  ASSERT_TRUE(until([&] { return fs::exists(children[0].result); }));
  EXPECT_EQ(fs::last_write_time(index()), time);
  EXPECT_FALSE(fs::exists(marker()));
}

TEST_F(Backend, InitialWindowAllowsConnectionAfterMarkerEstablished) {
  ASSERT_TRUE(std::holds_alternative<folio::application::LaunchedProcess>(launch()));
  ASSERT_TRUE(until([&] { return fs::exists(marker()); }));
  EXPECT_FALSE(fs::exists(children[0].result));
  {
    auto result = folio::application::BackendConnectionHandle::connect(
        std::get<app::ConnectionSettings>(app::connection_settings(paths)));
    ASSERT_TRUE(std::holds_alternative<folio::application::BackendConnectionHandle>(result));
  }
}

TEST_F(Backend, FailedIndexSaveRetainsMarkerAndReportsFailure) {
  {
    auto connection = connect();
    fs::create_directories(paths.application.cache);
    write(paths.application.cache / "index", "blocks index directory");
  }
  ASSERT_TRUE(until([&] { return fs::exists(children[0].result); }));
  EXPECT_EQ(read(children[0].result), "1");
  EXPECT_TRUE(fs::exists(marker()));
}

TEST_F(Backend, TwoFrontendsDisconnectBeforeFailedFinalizationReleasesOwnership) {
  {
    auto first = connect();
    {
      auto second = connect();
      EXPECT_TRUE(std::holds_alternative<std::monostate>(second.ping()));
      fs::create_directories(paths.application.cache);
      write(paths.application.cache / "index", "blocks index directory");
    }
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.ping()));
    EXPECT_FALSE(fs::exists(children.front().result));
  }
  ASSERT_TRUE(until([&] { return fs::exists(children.front().result); }));
  EXPECT_EQ(read(children.front().result), "1");
  EXPECT_TRUE(fs::exists(marker()));
  auto acquired = app::RepositoryOwnership::acquire(paths);
  EXPECT_TRUE(std::holds_alternative<app::RepositoryOwnership>(acquired));
}

TEST_F(Backend, CrashReleasesOwnershipAndRestartRebuildsDespiteValidStaleIndex) {
  clean_cache();
  {
    auto connection = connect();
    terminate(children[0].id);
    children[0].killed = true;
    ASSERT_TRUE(until([&] { return !connection.connected(); }));
    EXPECT_TRUE(std::holds_alternative<app::IpcError>(connection.ping()));
    EXPECT_TRUE(fs::exists(marker()));
  }
  storage::NormalRepository repo(paths.normal_root);
  folio::core::Memo memo("after crash", "",
                         {std::chrono::system_clock::now(), "UTC", std::nullopt});
  ASSERT_TRUE(
      std::holds_alternative<std::monostate>(repo.create_memo(memo.snapshot())));
  {
    auto restarted = connect();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(restarted.ping()));
  }
  ASSERT_TRUE(until([&] { return fs::exists(children.back().result); }));
  auto decoded = storage::index_codec::decode_index(read(index()));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  EXPECT_TRUE(std::get<storage::MetadataCatalog>(decoded).contains_entry(memo.id()));
  EXPECT_FALSE(fs::exists(marker()));
}

TEST_F(Backend, StartupFailureNeverSendsReadyAndRetainsPreviousMarker) {
  fs::create_directories(paths.application.state);
  write(marker());
  fs::create_directories(paths.normal_root / "memos");
  write(paths.normal_root / "memos/broken.md", "invalid");
  auto result = folio::application::connect_or_start_backend(
      std::get<app::ConnectionSettings>(app::connection_settings(paths)),
      [&] { return launch(); }, 5s);
  ASSERT_TRUE(std::holds_alternative<app::IpcError>(result));
  EXPECT_NE(std::get<app::IpcError>(result).message.find("initialization failed"),
            std::string::npos);
  EXPECT_TRUE(fs::exists(marker()));
  EXPECT_FALSE(fs::exists(index()));
}

TEST_F(Backend, ValidCacheDoesNotScanMalformedAuthoritativeFile) {
  clean_cache();
  fs::create_directories(paths.normal_root / "memos");
  write(paths.normal_root / "memos/broken.md", "invalid");
  {
    auto connection = connect();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  }
}

TEST_F(Backend, InvalidFramesRejectedWithoutTerminatingOtherClient) {
  auto keeper = connect();
  auto endpoint = std::get<app::ConnectionSettings>(app::connection_settings(paths)).endpoint;
  for (const auto& bytes :
       {app::encode_message({app::MessageKind::Hello, {}, 2}),
        std::string("\x00\x02\x00\x00", 4), std::string("\0\0\0\1", 4),
        app::encode_message(
            {app::MessageKind::Hello, app::payload_bytes("unexpected handshake payload")})}) {
    auto connected = app::IpcStream::connect(endpoint);
    ASSERT_TRUE(std::holds_alternative<app::IpcStream>(connected));
    auto stream = std::get<app::IpcStream>(std::move(connected));
    ASSERT_TRUE(std::holds_alternative<std::size_t>(
        stream.write(std::as_bytes(std::span(bytes.data(), bytes.size())))));
    std::string incoming;
    ASSERT_TRUE(until([&] {
      std::array<std::byte, 1024> buffer{};
      auto result = stream.read(buffer);
      if (std::holds_alternative<app::IpcError>(result))
        return true;
      incoming +=
          app::payload_text(std::span(buffer).first(std::get<std::size_t>(result)));
      return incoming.size() >= 8;
    }));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(keeper.ping()));
  }
}

TEST_F(Backend, ConcurrentCandidatesDoNotTouchWinnersSession) {
  auto connection = connect();
  const auto time = fs::last_write_time(marker());
  ASSERT_TRUE(std::holds_alternative<folio::application::LaunchedProcess>(launch()));
  ASSERT_TRUE(until([&] { return fs::exists(children.back().result); }));
  EXPECT_EQ(read(children.back().result), "4");
  EXPECT_EQ(fs::last_write_time(marker()), time);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
}

TEST_F(Backend, ConnectDuringLastDisconnectCanRetryNewOwner) {
  {
    auto connection = connect();
  }
  {
    auto next = connect();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(next.ping()));
  }
  ASSERT_TRUE(until([&] { return !fs::exists(marker()); }));
}
#ifndef _WIN32
TEST_F(Backend, StaleSocketReclaimedAndUnsafeEndpointRejected) {
  fs::create_directory(*paths.application.runtime);
  chmod(paths.application.runtime->c_str(), 0700);
  auto endpoint = std::get<app::ConnectionSettings>(app::connection_settings(paths)).endpoint;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strcpy(address.sun_path, endpoint.c_str());
  ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  chmod(endpoint.c_str(), 0600);
  close(fd);
  {
    auto connection = connect();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(connection.ping()));
  }
}

TEST_F(Backend, UnsafeRuntimePermissionsAreNotSilentlyAccepted) {
  fs::create_directory(*paths.application.runtime);
  chmod(paths.application.runtime->c_str(), 0777);
  auto result = folio::application::BackendConnectionHandle::connect(
      std::get<app::ConnectionSettings>(app::connection_settings(paths)));
  ASSERT_TRUE(std::holds_alternative<app::IpcError>(result));
  EXPECT_EQ(std::get<app::IpcError>(result).code, app::IpcErrorCode::AccessDenied);
}
#endif
TEST_F(Backend, ConfigurationAcceptsOverridesAndCreatesNothing) {
  fs::create_directory(paths.application.config);
  auto config = paths.application.config / "folio_config.toml";
  write(config, "[general.normal_storage]\nuse_default_location = false\npath = '" + paths.normal_root.generic_string() +
                    "'\n[general.vault_storage]\nuse_default_location = false\npath = '" + paths.vault_root.generic_string() + "'\n");
  auto result = folio::config::load_configuration(paths.application, config);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(result));
  EXPECT_FALSE(fs::exists(paths.normal_root));
  EXPECT_FALSE(fs::exists(paths.application.state));
  write(config, "[general.normal_storage]\nuse_default_location = false\npath = 'relative'\n[general.vault_storage]\nuse_default_location = false\npath = 'relative2'\n");
  EXPECT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(
      folio::config::load_configuration(paths.application, config)));
  EXPECT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(
      folio::config::load_configuration(paths.application, base / "missing")));
}

TEST(IpcProtocol, BinaryPayloadRoundTrip) {
  std::vector<std::byte> payload{std::byte{0}, std::byte{0xff}, std::byte{0x41}};
  auto encoded = app::encode_message({app::MessageKind::Hello, payload});
  auto decoded = app::decode_message(encoded);
  ASSERT_TRUE(std::holds_alternative<std::optional<app::Message>>(decoded));
  auto& message = std::get<std::optional<app::Message>>(decoded);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->payload, payload);
  EXPECT_TRUE(encoded.empty());
}

TEST(IpcProtocol, PartialFramesAndBounds) {
  auto full = app::encode_message({app::MessageKind::Ping, {}});
  std::string bytes = full.substr(0, 3);
  auto partial = app::decode_message(bytes);
  ASSERT_TRUE(std::holds_alternative<std::optional<app::Message>>(partial));
  EXPECT_FALSE(std::get<std::optional<app::Message>>(partial));
  bytes += full.substr(3);
  auto complete = app::decode_message(bytes);
  ASSERT_TRUE(std::holds_alternative<std::optional<app::Message>>(complete));
  ASSERT_TRUE(std::get<std::optional<app::Message>>(complete));
  EXPECT_TRUE(bytes.empty());
  bytes = std::string("\xff\xff\xff\xff", 4);
  EXPECT_TRUE(std::holds_alternative<app::IpcError>(app::decode_message(bytes)));
}
}  // namespace
