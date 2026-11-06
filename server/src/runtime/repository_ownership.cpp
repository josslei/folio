#include <folio/io/filesystem.hpp>
#include "runtime/repository_ownership.hpp"
#include <folio/ipc/ipc.hpp>

#include <utility>
#include <vector>

#ifndef _WIN32  // POSIX (Linux and macOS)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#else  // Windows
#define NOMINMAX
#include <windows.h>
#endif

namespace folio::application::internal {
namespace fs = std::filesystem;
namespace storage = core::storage;

struct RepositoryOwnership::Handles {
  std::optional<IpcListener> listener;
#ifndef _WIN32  // POSIX (Linux and macOS)
  std::vector<int> values;

  ~Handles() {
    listener.reset();
    for (auto fd : values)
      close(fd);
  }
#else  // Windows
  std::vector<HANDLE> values;

  ~Handles() {
    listener.reset();
    for (auto handle : values)
      CloseHandle(handle);
  }
#endif
};

RepositoryOwnership::RepositoryOwnership(std::unique_ptr<Handles> handles)
    : handles_(std::move(handles)) {
}

RepositoryOwnership::~RepositoryOwnership() = default;
RepositoryOwnership::RepositoryOwnership(RepositoryOwnership&&) noexcept = default;
RepositoryOwnership& RepositoryOwnership::operator=(RepositoryOwnership&&) noexcept =
    default;

namespace {
ProcessError io_error(std::string message, std::error_code ec) {
  return {ProcessErrorCode::IoError, std::move(message), ec};
}

bool contains(const fs::path& parent, const fs::path& child) {
  auto p = parent.begin();
  auto c = child.begin();
  for (; p != parent.end(); ++p, ++c) {
    if (c == child.end())
      return false;
#ifndef _WIN32  // POSIX (Linux and macOS)
    if (*p != *c)
      return false;
#else  // Windows
    if (CompareStringOrdinal(p->c_str(), -1, c->c_str(), -1, TRUE) != CSTR_EQUAL)
      return false;
#endif
  }
  return true;
}
}  // namespace

core::Result<RepositoryOwnership, ProcessError> RepositoryOwnership::acquire(
    const storage::StoragePaths& paths) {
  auto valid = storage::validate_storage_paths(paths);
  if (auto* error = std::get_if<io::FileError>(&valid))
    return ProcessError{ProcessErrorCode::InvalidArgument, error->message,
                        error->system_error};
  std::error_code ec;
  auto state = fs::weakly_canonical(paths.application.state, ec);
  if (ec)
    return io_error("Could not resolve State", ec);
  for (const auto& transient :
       {std::optional<fs::path>{paths.application.cache}, paths.application.runtime}) {
    if (!transient)
      continue;
    auto root = fs::weakly_canonical(*transient, ec);
    if (ec)
      return io_error("Could not resolve transient path", ec);
    if (contains(root, state / "backend.lock"))
      return ProcessError{ProcessErrorCode::InvalidArgument,
                          "Backend lock must be outside Cache and Runtime",
                          {}};
  }
  auto normal = fs::weakly_canonical(paths.normal_root, ec);
  if (ec)
    return io_error("Could not resolve Normal root", ec);
  auto vault = fs::weakly_canonical(paths.vault_root, ec);
  if (ec)
    return io_error("Could not resolve Vault root", ec);

  // Resolve supported aliases first, then use no-follow lifecycle helpers for
  // directory creation. External root renaming/replacement is not supported.
  for (const auto& directory : {state, normal}) {
    auto created = io::create_directories_durable(directory);
    if (auto* error = std::get_if<io::FileError>(&created))
      return io_error(error->message, error->system_error);
  }
  std::vector<fs::path> targets{state / "backend.lock", normal / ".folio-backend.lock"};
  const bool vault_exists = fs::exists(vault, ec);
  if (ec)
    return io_error("Could not inspect Vault root", ec);
  if (vault_exists)
    targets.push_back(vault / ".folio-backend.lock");
  auto handles = std::make_unique<Handles>();
  handles->values.reserve(targets.size());
  for (const auto& target : targets) {
#ifndef _WIN32  // POSIX (Linux and macOS)
    const int fd = open(target.c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd == -1)
      return io_error("Could not open Backend lock", {errno, std::generic_category()});
    handles->values.push_back(fd);
    struct stat info{};
    if (fstat(fd, &info) == -1)
      return io_error("Could not inspect Backend lock",
                      {errno, std::generic_category()});
    // Refuse foreign-user or hard-linked targets instead of trusting arbitrary
    // files. Cross-user shared storage is not a supported multi-user service.
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != geteuid())
      return ProcessError{
          ProcessErrorCode::InvalidArgument, "Unsafe Backend lock file", {}};
    int status;
    do {
      status = flock(fd, LOCK_EX | LOCK_NB);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
      const int code = errno;
      return ProcessError{code == EWOULDBLOCK || code == EAGAIN
                              ? ProcessErrorCode::AlreadyOwned
                              : ProcessErrorCode::IoError,
                          "Could not acquire Backend ownership",
                          {code, std::generic_category()}};
    }
#else  // Windows
    // No sharing for deletion: a live lock target cannot be replaced. Handles
    // are non-inheritable; LockFileEx provides the ownership test.
    HANDLE handle =
        CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      return io_error("Could not open Backend lock",
                      {static_cast<int>(GetLastError()), std::system_category()});
    handles->values.push_back(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
      return io_error("Could not inspect Backend lock",
                      {static_cast<int>(GetLastError()), std::system_category()});
    if (info.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      return ProcessError{
          ProcessErrorCode::InvalidArgument, "Backend lock must be a regular file", {}};
    OVERLAPPED overlap{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                    0, &overlap)) {
      const auto code = GetLastError();
      return ProcessError{code == ERROR_LOCK_VIOLATION ? ProcessErrorCode::AlreadyOwned
                                                       : ProcessErrorCode::IoError,
                          "Could not acquire Backend ownership",
                          {static_cast<int>(code), std::system_category()}};
    }
#endif
  }
  auto settings = connection_settings(paths);
  if (auto* error = std::get_if<IpcError>(&settings))
    return ProcessError{ProcessErrorCode::IoError, error->message, error->system_error};
  auto listener = IpcListener::listen(std::get<ConnectionSettings>(settings).endpoint);
  if (auto* error = std::get_if<IpcError>(&listener))
    return ProcessError{ProcessErrorCode::IoError, error->message, error->system_error};
  handles->listener.emplace(std::get<IpcListener>(std::move(listener)));
  return RepositoryOwnership{std::move(handles)};
}

core::Result<std::optional<IpcStream>, IpcError>
RepositoryOwnership::accept() {
  if (!handles_ || !handles_->listener)
    return IpcError{IpcErrorCode::Unavailable, "Backend is not listening"};
  return handles_->listener->accept();
}

core::Result<bool, IpcError> RepositoryOwnership::wait(
    std::chrono::steady_clock::time_point deadline, std::stop_token wake) {
  if (!handles_ || !handles_->listener)
    return IpcError{IpcErrorCode::Unavailable, "Backend listener is closed"};
  return handles_->listener->wait(deadline, wake);
}

void RepositoryOwnership::stop_listening() noexcept {
  if (handles_)
    handles_->listener.reset();
}
}  // namespace folio::application::internal
