#include "process.hpp"
#include "runtime/repository_ownership.hpp"
#include "backend.hpp"
#include "runtime/backend_states.hpp"
#include "folio/core/types.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <thread>
#include <type_traits>
#ifndef _WIN32  // POSIX (Linux and macOS)
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#else  // Windows
#define NOMINMAX
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace app = folio::application::internal;
namespace storage = folio::core::storage;
using namespace std::chrono_literals;

void touch(const fs::path& path, const std::string& value = "") {
  std::ofstream output(path);
  output << value;
}
std::string read(const fs::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>{input}, {}};
}
template <class Predicate>
bool until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

class Processes : public testing::Test {
 protected:
  fs::path base;
  storage::StoragePaths paths;
  struct Child { std::uint64_t id; fs::path control; };
  std::vector<Child> children;
  void SetUp() override {
#ifdef __APPLE__
    // macOS per-user temporary paths can consume most of sockaddr_un.sun_path.
    const auto temporary = fs::path("/private/tmp");
#else
    const auto temporary = fs::temp_directory_path();
#endif
    base = temporary / ("fp-" + folio::core::Id::generate().to_string().substr(0, 8));
    ASSERT_TRUE(fs::create_directory(base));
    base = fs::canonical(base);
    paths = {base / "normal", base / "vault",
        {base / "config", base / "data", base / "state", base / "cache", base / "logs", base / "runtime"}};
  }
  void TearDown() override {
    for (const auto& child : children) {
      touch(child.control / "release");
      touch(child.control / "go");
    }
    // Owned children acknowledge release after closing ownership handles. Losers
    // already exited. Every helper also has a bounded self-exit deadline.
    for (const auto& child : children) {
      EXPECT_TRUE(until([&] { return fs::exists(child.control / "result"); }));
      if (read(child.control / "result") == "owned" && !fs::exists(child.control / "killed"))
        EXPECT_TRUE(until([&] { return fs::exists(child.control / "released"); }));
    }
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }
  Child spawn(const fs::path& config_base = {}, const fs::path& normal = {}) {
    auto control = base / ("child-" + std::to_string(children.size()));
    fs::create_directory(control);
    auto result = app::launch_process(fs::path{FOLIO_PROCESS_CHILD},
        {config_base.empty() ? base : config_base, control, normal.empty() ? paths.normal_root : normal});
    if (auto* error = std::get_if<folio::application::ProcessError>(&result))
      throw std::runtime_error(error->message + ": " + error->system_error.message());
    Child child{std::get<folio::application::LaunchedProcess>(result).id, control};
    children.push_back(child);
    if (!until([&] { return fs::exists(control / "started"); }))
      throw std::runtime_error("Child did not start");
    return child;
  }
  std::string acquire(const Child& child) {
    touch(child.control / "go");
    if (!until([&] { return fs::exists(child.control / "result"); }))
      throw std::runtime_error("Child did not report ownership");
    return read(child.control / "result");
  }
};

TEST_F(Processes, LaunchReportsExecFailureAndResolvesActualExecutable) {
  auto executable = app::current_executable();
  ASSERT_TRUE(std::holds_alternative<fs::path>(executable));
  EXPECT_TRUE(fs::is_regular_file(std::get<fs::path>(executable)));
  auto result = app::launch_process(base / "does-not-exist", {});
  ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(result));
  EXPECT_EQ(std::get<folio::application::ProcessError>(result).code, folio::application::ProcessErrorCode::LaunchFailed);
  EXPECT_TRUE(std::get<folio::application::ProcessError>(result).system_error);
  EXPECT_TRUE(std::holds_alternative<folio::application::ProcessError>(app::launch_process("relative", {})));
}

TEST_F(Processes, BackendCanOutliveItsLaunchingProcess) {
  auto control = base / "child-0";
  fs::create_directory(control);
  auto result = app::launch_process(fs::path{FOLIO_PROCESS_CHILD},
      {"--test-launch-child", base, control, paths.normal_root});
  ASSERT_TRUE(std::holds_alternative<folio::application::LaunchedProcess>(result));
  ASSERT_TRUE(until([&] { return fs::exists(control / "launcher-result"); }));
  Child child{std::stoull(read(control / "launcher-result")), control};
  children.push_back(child);
  const auto launcher = std::get<folio::application::LaunchedProcess>(result).id;
#ifndef _WIN32  // POSIX (Linux and macOS)
  ASSERT_TRUE(until([&] { return kill(static_cast<pid_t>(launcher), 0) == -1 && errno == ESRCH; }));
#else  // Windows
  HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(launcher));
  if (handle) {
    EXPECT_EQ(WaitForSingleObject(handle, 8000), WAIT_OBJECT_0);
    CloseHandle(handle);
  }
#endif
  EXPECT_EQ(acquire(child), "owned");
}

TEST_F(Processes, OwnershipIsMoveOnlyAndLockFilesSurviveRelease) {
  static_assert(!std::is_copy_constructible_v<app::RepositoryOwnership>);
  {
    auto first = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(first));
    auto owner = std::move(std::get<app::RepositoryOwnership>(first));
    auto second = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(second));
    EXPECT_EQ(std::get<folio::application::ProcessError>(second).code, folio::application::ProcessErrorCode::AlreadyOwned);
    EXPECT_FALSE(fs::exists(paths.vault_root));
  }
  EXPECT_TRUE(fs::is_regular_file(paths.application.state / "backend.lock"));
  EXPECT_TRUE(fs::is_regular_file(paths.normal_root / ".folio-backend.lock"));
  EXPECT_TRUE(std::holds_alternative<app::RepositoryOwnership>(app::RepositoryOwnership::acquire(paths)));
}

TEST_F(Processes, IndependentProcessHoldsOwnershipUntilExplicitRelease) {
  auto child = spawn();
  ASSERT_EQ(acquire(child), "owned");
  auto result = app::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(result));
  EXPECT_EQ(std::get<folio::application::ProcessError>(result).code, folio::application::ProcessErrorCode::AlreadyOwned);
  touch(child.control / "release");
  ASSERT_TRUE(until([&] { return fs::exists(child.control / "released"); }));
  EXPECT_TRUE(std::holds_alternative<app::RepositoryOwnership>(app::RepositoryOwnership::acquire(paths)));
}

TEST_F(Processes, ConcurrentCandidatesHaveExactlyOneOwner) {
  auto a = spawn();
  auto b = spawn();
  touch(a.control / "go");
  touch(b.control / "go");
  ASSERT_TRUE(until([&] { return fs::exists(a.control / "result") && fs::exists(b.control / "result"); }));
  const auto ra = read(a.control / "result");
  const auto rb = read(b.control / "result");
  EXPECT_TRUE((ra == "owned" && rb == "busy") || (ra == "busy" && rb == "owned"));
}

TEST_F(Processes, ForcedProcessTerminationReleasesOwnership) {
  auto child = spawn();
  ASSERT_EQ(acquire(child), "owned");
#ifndef _WIN32  // POSIX (Linux and macOS)
  ASSERT_EQ(kill(static_cast<pid_t>(child.id), SIGKILL), 0);
#else  // Windows
  HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(child.id));
  ASSERT_NE(handle, nullptr);
  ASSERT_TRUE(TerminateProcess(handle, 9));
  EXPECT_EQ(WaitForSingleObject(handle, 8000), WAIT_OBJECT_0);
  CloseHandle(handle);
#endif
  touch(child.control / "killed");
  ASSERT_TRUE(until([&] { return std::holds_alternative<app::RepositoryOwnership>(app::RepositoryOwnership::acquire(paths)); }));
}

TEST_F(Processes, DifferentStateLocationsStillProtectSharedNormalRoot) {
  auto child = spawn();
  ASSERT_EQ(acquire(child), "owned");
  auto other = paths;
  other.application.state = base / "other-state";
  auto result = app::RepositoryOwnership::acquire(other);
  ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(result));
  EXPECT_EQ(std::get<folio::application::ProcessError>(result).code, folio::application::ProcessErrorCode::AlreadyOwned);
  // Failure on the root lock must release the earlier State lock.
  other.normal_root = base / "other-normal";
  EXPECT_TRUE(std::holds_alternative<app::RepositoryOwnership>(app::RepositoryOwnership::acquire(other)));
}

TEST_F(Processes, ExistingVaultIsProtectedButAbsentVaultIsNeverCreated) {
  fs::create_directory(paths.vault_root);
  auto first = app::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(first));
  auto other = paths;
  other.application.state = base / "other-state";
  other.normal_root = base / "other-normal";
  auto result = app::RepositoryOwnership::acquire(other);
  ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(result));
  EXPECT_EQ(std::get<folio::application::ProcessError>(result).code, folio::application::ProcessErrorCode::AlreadyOwned);
}

TEST_F(Processes, ProcessOwnershipNeverTouchesMarkerOrIndex) {
  fs::create_directories(paths.application.state);
  fs::create_directories(paths.application.cache / "index");
  auto marker = paths.application.state / "session.marker";
  auto index = paths.application.cache / "index/normal.msgpack";
  touch(marker, "existing session");
  touch(index, "existing index");
  const auto marker_time = fs::last_write_time(marker);
  const auto index_time = fs::last_write_time(index);
  {
    auto ownership = app::RepositoryOwnership::acquire(paths);
    ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(ownership));
  }
  EXPECT_EQ(read(marker), "existing session");
  EXPECT_EQ(read(index), "existing index");
  EXPECT_EQ(fs::last_write_time(marker), marker_time);
  EXPECT_EQ(fs::last_write_time(index), index_time);
  auto owner = app::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<app::RepositoryOwnership>(owner));
  EXPECT_EQ(app::run_backend(paths), 4);
}

TEST_F(Processes, UnsafeTargetsAndCacheLockLocationAreRejected) {
  auto invalid = paths;
  invalid.application.state = paths.application.cache / "state";
  EXPECT_TRUE(std::holds_alternative<folio::application::ProcessError>(app::RepositoryOwnership::acquire(invalid)));
  fs::create_directories(paths.application.state / "backend.lock");
  EXPECT_TRUE(std::holds_alternative<folio::application::ProcessError>(app::RepositoryOwnership::acquire(paths)));
}

TEST_F(Processes, NativeUnicodeAndSpaceArgumentsReachIndependentChild) {
  auto unicode = base / fs::path{u8"資料 with spaces"};
  fs::create_directory(unicode);
  auto child = spawn(unicode, unicode / "normal");
  EXPECT_EQ(acquire(child), "owned");
  EXPECT_TRUE(fs::is_regular_file(unicode / "state/backend.lock"));
}

#ifndef _WIN32  // POSIX
TEST_F(Processes, PhysicalRootAliasesContendAcrossProcesses) {
  fs::create_directory(paths.normal_root);
  auto alias = base / "alias";
  fs::create_directory_symlink(paths.normal_root, alias);
  auto child = spawn(base / "other-config", alias);
  ASSERT_EQ(acquire(child), "owned");
  auto result = app::RepositoryOwnership::acquire(paths);
  ASSERT_TRUE(std::holds_alternative<folio::application::ProcessError>(result));
  EXPECT_EQ(std::get<folio::application::ProcessError>(result).code, folio::application::ProcessErrorCode::AlreadyOwned);
}

TEST_F(Processes, LockTargetSymlinkIsRejectedWithoutChangingDestination) {
  fs::create_directories(paths.application.state);
  touch(base / "unrelated", "preserve");
  fs::create_symlink(base / "unrelated", paths.application.state / "backend.lock");
  EXPECT_TRUE(std::holds_alternative<folio::application::ProcessError>(app::RepositoryOwnership::acquire(paths)));
  EXPECT_EQ(read(base / "unrelated"), "preserve");
}

TEST_F(Processes, DetachedChildDoesNotInheritUnrelatedFileLocks) {
  const auto path = base / "unrelated-lock";
  const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600); // Intentionally inheritable.
  ASSERT_NE(fd, -1);
  ASSERT_EQ(flock(fd, LOCK_EX | LOCK_NB), 0);
  auto child = spawn();
  ASSERT_EQ(acquire(child), "owned");
  close(fd);
  const int fresh = open(path.c_str(), O_RDWR);
  ASSERT_NE(fresh, -1);
  EXPECT_EQ(flock(fresh, LOCK_EX | LOCK_NB), 0);
  close(fresh);
  // It is not our waitable child: the intermediate was already reaped.
  int status;
  EXPECT_EQ(waitpid(static_cast<pid_t>(child.id), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}
#endif
}  // namespace
