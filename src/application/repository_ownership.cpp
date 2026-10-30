#include "application/repository_ownership.hpp"

#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else  // POSIX (supported platforms: macOS and Linux)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace folio::application::internal {
namespace fs = std::filesystem;
namespace storage = core::storage;

struct RepositoryOwnership::Handles {
#ifdef _WIN32
  std::vector<HANDLE> values;
  ~Handles() { for (auto handle : values) CloseHandle(handle); }
#else  // POSIX (supported platforms: macOS and Linux)
  std::vector<int> values;
  ~Handles() { for (auto fd : values) close(fd); }
#endif
};
RepositoryOwnership::RepositoryOwnership(std::unique_ptr<Handles> handles)
    : handles_(std::move(handles)) {}
RepositoryOwnership::~RepositoryOwnership() = default;
RepositoryOwnership::RepositoryOwnership(RepositoryOwnership&&) noexcept = default;
RepositoryOwnership& RepositoryOwnership::operator=(RepositoryOwnership&&) noexcept = default;

namespace {
ProcessError io_error(std::string message, std::error_code ec) {
  return {ProcessErrorCode::IoError, std::move(message), ec};
}
bool contains(const fs::path& parent, const fs::path& child) {
  auto p = parent.begin();
  auto c = child.begin();
  for (; p != parent.end(); ++p, ++c) {
    if (c == child.end()) return false;
#ifdef _WIN32
    if (CompareStringOrdinal(p->c_str(), -1, c->c_str(), -1, TRUE) != CSTR_EQUAL)
      return false;
#else  // POSIX (supported platforms: macOS and Linux)
    if (*p != *c) return false;
#endif
  }
  return true;
}
}  // namespace

core::internal::Result<RepositoryOwnership, ProcessError>
RepositoryOwnership::acquire(const storage::StoragePaths& paths) {
  auto valid = storage::validate_storage_paths(paths);
  if (auto* error = std::get_if<storage::StorageError>(&valid))
    return ProcessError{ProcessErrorCode::InvalidArgument, error->message, error->system_error};
  std::error_code ec;
  auto state = fs::weakly_canonical(paths.application.state, ec);
  if (ec) return io_error("Could not resolve State", ec);
  for (const auto& transient : {std::optional<fs::path>{paths.application.cache},
                                paths.application.runtime}) {
    if (!transient) continue;
    auto root = fs::weakly_canonical(*transient, ec);
    if (ec) return io_error("Could not resolve transient path", ec);
    if (contains(root, state / "backend.lock"))
      return ProcessError{ProcessErrorCode::InvalidArgument,
                          "Backend lock must be outside Cache and Runtime", {}};
  }
  auto normal = fs::weakly_canonical(paths.normal_root, ec);
  if (ec) return io_error("Could not resolve Normal root", ec);
  auto vault = fs::weakly_canonical(paths.vault_root, ec);
  if (ec) return io_error("Could not resolve Vault root", ec);

  // Resolve supported aliases first, then use no-follow lifecycle helpers for
  // directory creation. External root renaming/replacement is not supported.
  for (const auto& directory : {state, normal}) {
    auto created = storage::create_directories_durable(directory);
    if (auto* error = std::get_if<storage::StorageError>(&created))
      return io_error(error->message, error->system_error);
  }
  std::vector<fs::path> targets{state / "backend.lock", normal / ".folio-backend.lock"};
  const bool vault_exists = fs::exists(vault, ec);
  if (ec) return io_error("Could not inspect Vault root", ec);
  if (vault_exists) targets.push_back(vault / ".folio-backend.lock");
  auto handles = std::make_unique<Handles>();
  handles->values.reserve(targets.size());
  for (const auto& target : targets) {
#ifdef _WIN32
    // No sharing for deletion: a live lock target cannot be replaced. Handles
    // are non-inheritable; LockFileEx provides the ownership test.
    HANDLE handle = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      return io_error("Could not open Backend lock", {static_cast<int>(GetLastError()), std::system_category()});
    handles->values.push_back(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
      return io_error("Could not inspect Backend lock", {static_cast<int>(GetLastError()), std::system_category()});
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      return ProcessError{ProcessErrorCode::InvalidArgument, "Backend lock must be a regular file", {}};
    OVERLAPPED overlap{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
      const auto code = GetLastError();
      return ProcessError{code == ERROR_LOCK_VIOLATION ? ProcessErrorCode::AlreadyOwned : ProcessErrorCode::IoError,
                          "Could not acquire Backend ownership", {static_cast<int>(code), std::system_category()}};
    }
#else  // POSIX (supported platforms: macOS and Linux)
    const int fd = open(target.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd == -1) return io_error("Could not open Backend lock", {errno, std::generic_category()});
    handles->values.push_back(fd);
    struct stat info{};
    if (fstat(fd, &info) == -1)
      return io_error("Could not inspect Backend lock", {errno, std::generic_category()});
    // Refuse foreign-user or hard-linked targets instead of trusting arbitrary
    // files. Cross-user shared storage is not a supported multi-user service.
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != geteuid())
      return ProcessError{ProcessErrorCode::InvalidArgument, "Unsafe Backend lock file", {}};
    int status;
    do { status = flock(fd, LOCK_EX | LOCK_NB); } while (status == -1 && errno == EINTR);
    if (status == -1) {
      const int code = errno;
      return ProcessError{code == EWOULDBLOCK || code == EAGAIN ? ProcessErrorCode::AlreadyOwned : ProcessErrorCode::IoError,
                          "Could not acquire Backend ownership", {code, std::generic_category()}};
    }
#endif
  }
  return RepositoryOwnership{std::move(handles)};
}
}  // namespace folio::application::internal
