#include "application/internal/process.hpp"

#include <limits>

#ifndef _WIN32  // POSIX (Linux and macOS)
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#else  // Windows
#define NOMINMAX
#include <windows.h>
#endif

namespace folio::application::internal {
namespace fs = std::filesystem;
namespace {
bool has_nul(const fs::path& path) {
  return path.native().find(fs::path::value_type{}) != fs::path::string_type::npos;
}
#ifndef _WIN32  // POSIX (Linux and macOS)
struct LaunchReport { int error; pid_t pid; };
// Only async-signal-safe operations in the forked children, through exec/_exit.
[[noreturn]] void child_failure(int fd, int error) {
  const LaunchReport report{error, 0};
  ssize_t count;
  do { count = write(fd, &report, sizeof report); } while (count == -1 && errno == EINTR);
  _exit(127);
}
#else  // Windows
// Windows argv quoting, including trailing backslashes before the closing quote.
std::wstring quote(const std::wstring& value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') { ++slashes; continue; }
    result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}
#endif
}

core::Result<fs::path, ProcessError> current_executable() {
#if defined(__linux__)
  std::vector<char> buffer(512);
  for (;;) {
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size == -1) return ProcessError{ProcessErrorCode::IoError, "Could not find current executable",
                                      {errno, std::generic_category()}};
    if (static_cast<std::size_t>(size) < buffer.size())
      return fs::path{std::string{buffer.data(), static_cast<std::size_t>(size)}};
    if (buffer.size() >= 1024 * 1024)
      return ProcessError{ProcessErrorCode::IoError, "Executable path is too long", {}};
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return ProcessError{ProcessErrorCode::IoError, "Could not find current executable", {}};
  std::error_code ec;
  auto path = fs::canonical(buffer.data(), ec);
  if (ec) return ProcessError{ProcessErrorCode::IoError, "Could not resolve executable", ec};
  return path;
#elif defined(_WIN32)
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) return ProcessError{ProcessErrorCode::IoError, "Could not find current executable",
                                     {static_cast<int>(GetLastError()), std::system_category()}};
    if (size < buffer.size()) return fs::path{std::wstring{buffer.data(), size}};
    if (buffer.size() >= 32768) return ProcessError{ProcessErrorCode::IoError, "Executable path is too long", {}};
    buffer.resize(buffer.size() * 2);
  }
#endif
}

core::Result<LaunchedProcess, ProcessError> launch_process(
    const fs::path& executable, const std::vector<fs::path>& arguments) {
  if (!executable.is_absolute() || has_nul(executable))
    return ProcessError{ProcessErrorCode::InvalidArgument, "Executable must be an absolute path without NUL", {}};
  for (const auto& argument : arguments)
    if (has_nul(argument))
      return ProcessError{ProcessErrorCode::InvalidArgument, "Argument contains NUL", {}};
#ifndef _WIN32  // POSIX (Linux and macOS)
  std::vector<std::string> strings{executable.native()};
  for (const auto& argument : arguments) strings.push_back(argument.native());
  std::vector<char*> argv;
  for (auto& value : strings) argv.push_back(value.data());
  argv.push_back(nullptr);
  const char* program = strings.front().c_str();
  char* const* args = argv.data();
  char* const* env = environ;
  struct rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) == -1)
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not inspect descriptor limit", {errno, std::generic_category()}};
  const auto upper = limit.rlim_max == RLIM_INFINITY ? sysconf(_SC_OPEN_MAX) :
      static_cast<long>(limit.rlim_max > INT_MAX ? INT_MAX : limit.rlim_max);
  if (upper < 0) return ProcessError{ProcessErrorCode::LaunchFailed, "Invalid descriptor limit", {}};
  int pipe_fds[2];
  if (pipe(pipe_fds) == -1)
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not create exec error pipe", {errno, std::generic_category()}};
  // Keep the reporting descriptor above stdin/out/err even in headless callers.
  const int report_fd = fcntl(pipe_fds[1], F_DUPFD_CLOEXEC, 3);
  close(pipe_fds[1]);
  if (report_fd == -1) {
    const int code = errno;
    close(pipe_fds[0]);
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not prepare exec error pipe", {code, std::generic_category()}};
  }
  sigset_t blocked, previous;
  sigfillset(&blocked);
  const int mask_error = pthread_sigmask(SIG_SETMASK, &blocked, &previous);
  if (mask_error) {
    close(pipe_fds[0]); close(report_fd);
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not block launch signals", {mask_error, std::generic_category()}};
  }
  const pid_t intermediate = fork();
  const int fork_error = errno;
  if (intermediate == 0) {
    // No allocation, C++ runtime work or inherited application execution here.
    close(pipe_fds[0]);
    if (setsid() == -1) child_failure(report_fd, errno);
    const pid_t child = fork();
    if (child == -1) child_failure(report_fd, errno);
    if (child != 0) _exit(0);
    for (long fd = 0; fd < upper; ++fd)
      if (fd != report_fd) close(static_cast<int>(fd));
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd == -1) child_failure(report_fd, errno);
    for (int fd = 0; fd < 3; ++fd)
      if (dup2(null_fd, fd) == -1) child_failure(report_fd, errno);
    if (null_fd > 2) close(null_fd);
    if (chdir("/") == -1) child_failure(report_fd, errno);
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int sig = 1; sig < NSIG; ++sig)
      if (sig != SIGKILL && sig != SIGSTOP) sigaction(sig, &action, nullptr);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
    const LaunchReport report{0, getpid()};
    ssize_t count;
    do { count = write(report_fd, &report, sizeof report); } while (count == -1 && errno == EINTR);
    if (count != sizeof report) _exit(127);
    execve(program, args, env);
    child_failure(report_fd, errno);
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  close(report_fd);
  if (intermediate == -1) {
    close(pipe_fds[0]);
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not fork launcher", {fork_error, std::generic_category()}};
  }
  // Reap only our short-lived intermediate; the detached process belongs to the
  // OS reaper. No SIGCHLD policy changes, background reaper thread or leaked child.
  int status;
  while (waitpid(intermediate, &status, 0) == -1 && errno == EINTR) {}
  LaunchReport report{};
  std::uint64_t id = 0;
  int failure = 0;
  for (;;) {
    std::size_t offset = 0;
    while (offset < sizeof report) {
      const auto count = read(pipe_fds[0], reinterpret_cast<char*>(&report) + offset, sizeof report - offset);
      if (count == -1 && errno == EINTR) continue;
      if (count < 0) { failure = errno; break; }
      if (count == 0) break;
      offset += static_cast<std::size_t>(count);
    }
    if (failure || offset == 0) break;
    if (offset != sizeof report) { failure = EIO; break; }
    if (report.error) { failure = report.error; break; }
    id = static_cast<std::uint64_t>(report.pid);
  }
  close(pipe_fds[0]);
  if (failure || !id)
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not execute detached process",
                        {failure ? failure : EIO, std::generic_category()}};
  return LaunchedProcess{id};
#else  // Windows
  auto command = quote(executable.native());
  for (const auto& argument : arguments) command += L" " + quote(argument.native());
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  // DETACHED_PROCESS prevents terminal input ownership; FALSE prevents handle
  // inheritance. No wait or service/readiness claim accompanies this result.
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                      &startup, &process))
    return ProcessError{ProcessErrorCode::LaunchFailed, "Could not launch process",
                        {static_cast<int>(GetLastError()), std::system_category()}};
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return LaunchedProcess{process.dwProcessId};
#endif
}
}  // namespace folio::application::internal
