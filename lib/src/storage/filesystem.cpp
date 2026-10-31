#include "storage/filesystem.hpp"
#include "storage/atomic_write.hpp"
#include "storage/filesystem_durability_test_hooks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace folio::core::storage {
namespace {

namespace fs = std::filesystem;

StorageError error(StorageErrorCode code, const fs::path& path,
                   std::string message, std::error_code system_error = {},
                   bool replacement_completed = false) {
  return {code, path, std::move(message), system_error, replacement_completed};
}

bool has_nul(const fs::path& path) {
  return path.native().find(fs::path::value_type{}) != fs::path::string_type::npos;
}

bool component_equal(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
  const auto& a = left.native();
  const auto& b = right.native();
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
  return left == right;
#endif
}

bool contains_path(const fs::path& parent, const fs::path& child) {
  auto p = parent.begin();
  auto c = child.begin();
  for (; p != parent.end(); ++p, ++c) {
    if (c == child.end() || !component_equal(*p, *c)) return false;
  }
  return true;
}

bool overlaps(const fs::path& a, const fs::path& b) {
  return contains_path(a, b) || contains_path(b, a);
}

fs::path temporary_prefix(const fs::path& destination) {
  fs::path prefix{"."};
  prefix += destination.filename();
  prefix += ".tmp-";
  return prefix;
}

fs::path temporary_name(const fs::path& destination) {
  auto name = temporary_prefix(destination);
  name += core::Id::generate().to_string();
  return name;
}

::folio::core::Result<std::monostate, StorageError> check_pending_temporary(const fs::path& parent,
                                              const fs::path& destination) {
  const auto prefix = temporary_prefix(destination).native();
  std::error_code ec;
  fs::directory_iterator item{parent, ec};
  const fs::directory_iterator end;
  while (!ec && item != end) {
    const auto name = item->path().filename().native();
    if (name.size() >= prefix.size() &&
        component_equal(fs::path{name.substr(0, prefix.size())}, fs::path{prefix})) {
      return error(StorageErrorCode::RecoveryRequired, destination,
                   "An existing temporary file must be resolved before writing this entry");
    }
    item.increment(ec);
  }
  if (ec) return error(StorageErrorCode::IoError, destination,
                       "Could not inspect temporary files", ec);
  return std::monostate{};
}

#ifdef _WIN32

std::error_code system_error() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

struct TemporaryFile {
  HANDLE handle = INVALID_HANDLE_VALUE;
  fs::path path;
  bool owned = false;

  ~TemporaryFile() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (owned) DeleteFileW(path.c_str());
  }
};

struct DirectoryLock {
  HANDLE handle = nullptr;
  bool acquired = false;

  ~DirectoryLock() {
    if (acquired) ReleaseMutex(handle);
    if (handle) CloseHandle(handle);
  }
};

#else

std::error_code system_error() { return {errno, std::generic_category()}; }

struct FileDescriptor {
  int value = -1;
  ~FileDescriptor() {
    if (value != -1) close(value);
  }
};

struct TemporaryFile {
  int directory;
  int descriptor = -1;
  std::string name;
  bool owned = false;

  ~TemporaryFile() {
    if (descriptor != -1) close(descriptor);
    if (owned) unlinkat(directory, name.c_str(), 0);
  }
};

bool sync_file(int descriptor) {
  int status;
  do {
    status = fsync(descriptor);
  } while (status == -1 && errno == EINTR);
  return status == 0;
}

#endif

}  // namespace

::folio::core::Result<StoragePaths, StorageError> validate_storage_paths(const StoragePaths& paths) {
  std::vector<fs::path> roots{paths.normal_root, paths.vault_root,
      paths.application.config, paths.application.data,
      paths.application.state, paths.application.cache};
  if (paths.application.runtime) roots.push_back(*paths.application.runtime);
  std::vector<fs::path> resolved(roots.size());
  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].empty() || !roots[i].is_absolute() || has_nul(roots[i])) {
      return error(StorageErrorCode::InvalidPath, roots[i],
                   "Storage roots must be nonempty absolute paths without NUL bytes");
    }
    std::error_code ec;
    resolved[i] = fs::weakly_canonical(roots[i], ec);
    if (ec) {
      return error(StorageErrorCode::IoError, roots[i], "Could not resolve storage root", ec);
    }
    if (resolved[i].has_relative_path() && resolved[i].filename().empty()) {
      resolved[i] = resolved[i].parent_path();
    }
    const auto status = fs::status(resolved[i], ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      return error(StorageErrorCode::IoError, roots[i], "Could not inspect storage root", ec);
    }
    if (fs::exists(status) && !fs::is_directory(status)) {
      return error(StorageErrorCode::InvalidPath, roots[i], "Storage root is not a directory");
    }
  }
  for (std::size_t i = 0; i < 2; ++i) {
    for (std::size_t j = i + 1; j < resolved.size(); ++j) {
      if (overlaps(resolved[i], resolved[j])) {
        return error(StorageErrorCode::InvalidPath, roots[j],
                     "Authoritative roots must not overlap each other or application directories");
      }
    }
  }
  return paths;
}

::folio::core::Result<std::string, StorageError> normal_record_filename(RecordType type, const core::Id& id,
                                         core::Timestamp created) {
  using namespace std::chrono;
  const char* extension;
  switch (type) {
    case RecordType::Log:
    case RecordType::Memo:
      extension = ".md";
      break;
    case RecordType::Todo:
      extension = ".toml";
      break;
    default:
      return error(StorageErrorCode::InvalidRecordType, {}, "Invalid entry type");
  }
  const auto whole_seconds = floor<seconds>(created);
  const auto day = floor<days>(whole_seconds);
  const year_month_day date{day};
  const auto year = static_cast<int>(date.year());
  if (!date.ok() || year < 0 || year > 9999) {
    return error(StorageErrorCode::InvalidTimestamp, {}, "Creation year is outside 0000 through 9999");
  }
  const hh_mm_ss time{whole_seconds - day};
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setfill('0') << std::setw(4) << year << '-'
         << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(date.day()) << '_'
         << std::setw(2) << time.hours().count()
         << std::setw(2) << time.minutes().count()
         << std::setw(2) << time.seconds().count() << '_' << id.to_string() << extension;
  return output.str();
}

namespace internal {

::folio::core::Result<std::monostate, StorageError> write_file_atomic(const fs::path& path,
                                        std::string_view contents,
                                        bool create_only, SyncFile sync) {
  if (path.empty() || has_nul(path) || path.filename().empty() ||
      path.filename() == "." || path.filename() == "..") {
    return error(StorageErrorCode::InvalidPath, path, "Expected a file path without NUL bytes");
  }
  const auto parent = path.has_parent_path() ? path.parent_path() : fs::path{"."};
#ifdef _WIN32
  TemporaryFile directory;
  directory.handle = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (directory.handle == INVALID_HANDLE_VALUE) {
    return error(StorageErrorCode::IoError, path, "Could not open parent directory", system_error());
  }
  BY_HANDLE_FILE_INFORMATION identity{};
  if (!GetFileInformationByHandle(directory.handle, &identity)) {
    return error(StorageErrorCode::IoError, path, "Could not identify parent directory", system_error());
  }
  const auto lock_name = L"Global\\FolioDirectoryWrite-" +
      std::to_wstring(identity.dwVolumeSerialNumber) + L"-" +
      std::to_wstring(identity.nFileIndexHigh) + L"-" + std::to_wstring(identity.nFileIndexLow);
  DirectoryLock lock;
  lock.handle = CreateMutexW(nullptr, FALSE, lock_name.c_str());
  if (!lock.handle) {
    return error(StorageErrorCode::IoError, path, "Could not open directory mutex", system_error());
  }
  const auto wait = WaitForSingleObject(lock.handle, 0);
  lock.acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
  if (!lock.acquired) {
    return error(StorageErrorCode::IoError, path, "Could not lock parent directory",
                 wait == WAIT_TIMEOUT ? std::make_error_code(std::errc::device_or_resource_busy)
                                      : system_error());
  }
  const auto pending = check_pending_temporary(parent, path);
  if (const auto* failure = std::get_if<StorageError>(&pending)) return *failure;
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
      return error(StorageErrorCode::InvalidPath, path, "Destination must be a regular file");
    }
    if (create_only) return error(StorageErrorCode::IoError, path, "Destination already exists",
                                  std::make_error_code(std::errc::file_exists));
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    return error(StorageErrorCode::IoError, path, "Could not inspect destination", system_error());
  } else if (!create_only) {
    return error(StorageErrorCode::IoError, path, "Destination does not exist",
                 std::make_error_code(std::errc::no_such_file_or_directory));
  }
  TemporaryFile temporary;
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary.path = parent / temporary_name(path);
    if (component_equal(temporary.path.filename(), path.filename())) continue;
    temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.handle != INVALID_HANDLE_VALUE) {
      temporary.owned = true;
      break;
    }
    const auto ec = system_error();
    if (ec.value() != ERROR_FILE_EXISTS && ec.value() != ERROR_ALREADY_EXISTS) {
      return error(StorageErrorCode::IoError, path, "Could not create temporary file", ec);
    }
  }
  if (!temporary.owned) {
    return error(StorageErrorCode::IoError, path, "Could not allocate an exclusive temporary file");
  }
  while (!contents.empty()) {
    const auto count = static_cast<DWORD>(std::min<std::size_t>(contents.size(),
                                                               std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(temporary.handle, contents.data(), count, &written, nullptr)) {
      return error(StorageErrorCode::IoError, path, "Could not write temporary file", system_error());
    }
    if (written == 0) {
      return error(StorageErrorCode::IoError, path, "Temporary file write made no progress");
    }
    contents.remove_prefix(written);
  }
  if (!FlushFileBuffers(temporary.handle)) {
    return error(StorageErrorCode::IoError, path, "Could not flush temporary file", system_error());
  }
  const auto handle = temporary.handle;
  temporary.handle = INVALID_HANDLE_VALUE;
  if (!CloseHandle(handle)) {
    return error(StorageErrorCode::IoError, path, "Could not close temporary file", system_error());
  }
  const auto move_flags = MOVEFILE_WRITE_THROUGH |
      (create_only ? 0 : MOVEFILE_REPLACE_EXISTING);
  if (!MoveFileExW(temporary.path.c_str(), path.c_str(), move_flags)) {
    return error(StorageErrorCode::IoError, path, "Could not replace destination", system_error());
  }
  temporary.owned = false;
#else
  FileDescriptor directory{open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (directory.value == -1) {
    return error(StorageErrorCode::IoError, path, "Could not open parent directory", system_error());
  }
  int locked;
  do {
    locked = flock(directory.value, LOCK_EX | LOCK_NB);
  } while (locked == -1 && errno == EINTR);
  if (locked == -1) {
    return error(StorageErrorCode::IoError, path, "Could not lock parent directory", system_error());
  }
  const auto pending = check_pending_temporary(parent, path);
  if (const auto* failure = std::get_if<StorageError>(&pending)) return *failure;
  const auto filename = path.filename().native();
  struct stat status{};
  if (fstatat(directory.value, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(status.st_mode)) {
      return error(StorageErrorCode::InvalidPath, path, "Destination must be a regular file");
    }
    if (create_only) return error(StorageErrorCode::IoError, path, "Destination already exists",
                                  std::make_error_code(std::errc::file_exists));
  } else if (errno != ENOENT) {
    return error(StorageErrorCode::IoError, path, "Could not inspect destination", system_error());
  } else if (!create_only) {
    return error(StorageErrorCode::IoError, path, "Destination does not exist",
                 std::make_error_code(std::errc::no_such_file_or_directory));
  }
  TemporaryFile temporary{directory.value};
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary.name = temporary_name(path).native();
    if (temporary.name == filename) continue;
    temporary.descriptor = openat(directory.value, temporary.name.c_str(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (temporary.descriptor != -1) {
      temporary.owned = true;
      break;
    }
    if (errno != EEXIST) {
      return error(StorageErrorCode::IoError, path, "Could not create temporary file", system_error());
    }
  }
  if (!temporary.owned) {
    return error(StorageErrorCode::IoError, path, "Could not allocate an exclusive temporary file");
  }
  while (!contents.empty()) {
    const auto count = std::min<std::size_t>(contents.size(),
                                            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto written = write(temporary.descriptor, contents.data(), count);
    if (written == -1) {
      if (errno == EINTR) continue;
      return error(StorageErrorCode::IoError, path, "Could not write temporary file", system_error());
    }
    if (written == 0) {
      return error(StorageErrorCode::IoError, path, "Temporary file write made no progress");
    }
    contents.remove_prefix(static_cast<std::size_t>(written));
  }
  if (!sync(temporary.descriptor)) {
    return error(StorageErrorCode::IoError, path, "Could not sync temporary file", system_error());
  }
  const auto descriptor = temporary.descriptor;
  temporary.descriptor = -1;
  // Do not retry close after EINTR: the descriptor may already have been released.
  if (close(descriptor) == -1) {
    return error(StorageErrorCode::IoError, path, "Could not close temporary file", system_error());
  }
  // Persist the staged filename before changing the destination.
  if (!sync(directory.value)) {
    const auto sync_error = system_error();
    if (unlinkat(directory.value, temporary.name.c_str(), 0) == -1) {
      return error(StorageErrorCode::IoError, path,
                   "Could not remove temporary file after staging sync failed", system_error());
    }
    temporary.owned = false;
    if (!sync(directory.value)) {
      return error(StorageErrorCode::DurabilityFailure, path,
                   "Temporary file removed but directory cleanup sync failed", system_error());
    }
    return error(StorageErrorCode::DurabilityFailure, path,
                 "Could not sync staged temporary file's directory; temporary file removed",
                 sync_error);
  }
  const auto replacement_status = create_only
      ? linkat(directory.value, temporary.name.c_str(), directory.value, filename.c_str(), 0)
      : renameat(directory.value, temporary.name.c_str(), directory.value, filename.c_str());
  if (replacement_status == -1) {
    return error(StorageErrorCode::IoError, path, "Could not publish destination", system_error());
  }
  // Rename consumes the temporary name; create leaves a link to the new file.
  if (!create_only) temporary.owned = false;
  if (!sync(directory.value)) {
    const auto sync_error = system_error();
    if (create_only) {
      if (unlinkat(directory.value, temporary.name.c_str(), 0) == -1) {
        return error(StorageErrorCode::DurabilityFailure, path,
                     "Publication sync failed and temporary link could not be removed",
                     system_error(), true);
      }
      temporary.owned = false;
      if (!sync(directory.value)) {
        return error(StorageErrorCode::DurabilityFailure, path,
                     "Temporary link removed but directory cleanup sync failed",
                     system_error(), true);
      }
    }
    return error(StorageErrorCode::DurabilityFailure, path,
                 "Publication completed but parent directory sync failed", sync_error, true);
  }
  if (create_only && unlinkat(directory.value, temporary.name.c_str(), 0) == -1) {
    return error(StorageErrorCode::IoError, path, "Created destination but could not remove temporary link",
                 system_error(), true);
  }
  temporary.owned = false;
  if (create_only && !sync(directory.value)) {
    return error(StorageErrorCode::DurabilityFailure, path,
                 "Creation is durable but temporary link removal could not be synced",
                 system_error(), true);
  }
#endif
  return std::monostate{};
}

}  // namespace internal

::folio::core::Result<std::monostate, StorageError> create_file_atomic(const fs::path& path, std::string_view contents) {
  return internal::write_file_atomic(path, contents, true,
#ifdef _WIN32
                                     nullptr);
#else
                                     sync_file);
#endif
}

::folio::core::Result<std::monostate, StorageError> replace_file_atomic(const fs::path& path, std::string_view contents) {
  return internal::write_file_atomic(path, contents, false,
#ifdef _WIN32
                                     nullptr);
#else
                                     sync_file);
#endif
}

::folio::core::Result<std::string, BinaryReadError> read_binary_file(const std::filesystem::path& path,
                                               std::size_t max_bytes) {
  if (path.empty() || path.native().find(std::filesystem::path::value_type{}) !=
                          std::filesystem::path::string_type::npos)
    return BinaryReadError{BinaryReadErrorCode::IoError, path,
                           "Invalid binary file path", std::make_error_code(std::errc::invalid_argument)};
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return BinaryReadError{BinaryReadErrorCode::IoError, path,
                                 "Could not measure binary file", ec};
  if (size > max_bytes)
    return BinaryReadError{BinaryReadErrorCode::LimitExceeded, path,
                           "Binary file exceeds byte limit", {}};
  std::ifstream input(path, std::ios::binary);
  if (!input) return BinaryReadError{BinaryReadErrorCode::IoError, path,
                                     "Could not open binary file", {}};
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(size));
  std::array<char, 64u * 1024u> chunk{};
  while (input) {
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = input.gcount();
    if (count > 0) {
      if (static_cast<std::size_t>(count) > max_bytes - bytes.size())
        return BinaryReadError{BinaryReadErrorCode::LimitExceeded, path,
                               "Binary file exceeds byte limit", {}};
      bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
  }
  if (input.bad() || (input.fail() && !input.eof()))
    return BinaryReadError{BinaryReadErrorCode::IoError, path,
                           "Could not read binary file", {}};
  return bytes;
}

namespace {
// Test-only sync override is thread-local and never installed by production.
thread_local internal::SyncFile lifecycle_test_sync = nullptr;
#ifndef _WIN32
bool lifecycle_sync(int descriptor) {
  return lifecycle_test_sync ? lifecycle_test_sync(descriptor) : sync_file(descriptor);
}
#endif

::folio::core::Result<std::monostate, StorageError>
validate_lifecycle_file_path(const fs::path& path) {
  if (!path.is_absolute() || has_nul(path) || path != path.lexically_normal() ||
      path.filename().empty() || path.filename() == "." || path.filename() == "..")
    return error(StorageErrorCode::InvalidPath, path, "Expected canonical absolute file path");
  auto current = path.root_path();
  for (const auto& component : path.parent_path().relative_path()) {
    current /= component;
    std::error_code ec;
    const auto status = fs::symlink_status(current, ec);
    if (ec) return error(StorageErrorCode::IoError, current, "Could not inspect file parent", ec);
    if (!fs::is_directory(status) || fs::is_symlink(status))
      return error(StorageErrorCode::InvalidPath, current, "Unsafe file parent");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
      return error(StorageErrorCode::IoError, current, "Could not inspect file parent", system_error());
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
      return error(StorageErrorCode::InvalidPath, current, "Unsafe file parent");
#endif
  }
  return std::monostate{};
}
}

namespace internal {
SyncFile set_lifecycle_sync_for_testing(SyncFile sync) noexcept {
  return std::exchange(lifecycle_test_sync, sync);
}
}

::folio::core::Result<std::monostate, StorageError>
create_directories_durable(const fs::path& path) {
  if (!path.is_absolute() || has_nul(path) || path != path.lexically_normal())
    return error(StorageErrorCode::InvalidPath, path, "Expected canonical absolute directory path");
#ifdef _WIN32
  auto current = path.root_path();
  for (const auto& component : path.relative_path()) {
    current /= component;
    std::error_code ec;
    fs::create_directory(current, ec);
    if (ec) return error(StorageErrorCode::IoError, current, "Could not create directory", ec);
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
      return error(StorageErrorCode::IoError, current, "Could not inspect directory", system_error());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return error(StorageErrorCode::InvalidPath, current, "Unsafe directory component");
  }
#else
  FileDescriptor directory{open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (directory.value == -1)
    return error(StorageErrorCode::IoError, path, "Could not open filesystem root", system_error());
  for (const auto& component : path.relative_path()) {
    if (component == "." || component == "..")
      return error(StorageErrorCode::InvalidPath, path, "Unsafe directory component");
    if (mkdirat(directory.value, component.c_str(), 0700) != 0 && errno != EEXIST)
      return error(StorageErrorCode::IoError, path, "Could not create directory", system_error());
    FileDescriptor child{openat(directory.value, component.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (child.value == -1) {
      const auto ec = system_error();
      const auto code = ec == std::errc::too_many_symbolic_link_levels ||
                        ec == std::errc::not_a_directory
          ? StorageErrorCode::InvalidPath : StorageErrorCode::IoError;
      return error(code, path, "Could not open safe directory component", ec);
    }
    if (!lifecycle_sync(directory.value))
      return error(StorageErrorCode::DurabilityFailure, path, "Could not sync directory link", system_error());
    std::swap(directory.value, child.value);
  }
  if (!lifecycle_sync(directory.value))
    return error(StorageErrorCode::DurabilityFailure, path, "Could not sync directory", system_error());
#endif
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
create_empty_file_durable(const fs::path& path) {
  auto valid = validate_lifecycle_file_path(path);
  if (auto* problem = std::get_if<StorageError>(&valid)) return *problem;
#ifdef _WIN32
  TemporaryFile file;
  file.handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.handle == INVALID_HANDLE_VALUE)
    return error(StorageErrorCode::IoError, path, "Could not exclusively create empty file", system_error());
  if (!FlushFileBuffers(file.handle))
    return error(StorageErrorCode::DurabilityFailure, path, "Empty file published but flush failed", system_error(), true);
#else
  FileDescriptor directory{open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.value == -1)
    return error(StorageErrorCode::IoError, path, "Could not open empty-file parent", system_error());
  FileDescriptor file{openat(directory.value, path.filename().c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (file.value == -1)
    return error(StorageErrorCode::IoError, path, "Could not exclusively create empty file", system_error());
  // No payload can tear: presence alone is significant. Never unlink on a failed
  // sync, since the next lifecycle must conservatively observe publication.
  if (!lifecycle_sync(file.value))
    return error(StorageErrorCode::DurabilityFailure, path, "Empty file published but sync failed", system_error(), true);
  if (!lifecycle_sync(directory.value))
    return error(StorageErrorCode::DurabilityFailure, path, "Empty file published but parent sync failed", system_error(), true);
#endif
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
sync_regular_file(const fs::path& path) {
  auto valid = validate_lifecycle_file_path(path);
  if (auto* problem = std::get_if<StorageError>(&valid)) return *problem;
#ifdef _WIN32
  TemporaryFile file;
  file.handle = CreateFileW(path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file.handle == INVALID_HANDLE_VALUE)
    return error(StorageErrorCode::IoError, path, "Could not open file for sync", system_error());
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file.handle, &info))
    return error(StorageErrorCode::IoError, path, "Could not inspect file", system_error());
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    return error(StorageErrorCode::InvalidPath, path, "Expected regular file");
  if (!FlushFileBuffers(file.handle))
    return error(StorageErrorCode::DurabilityFailure, path, "Could not flush file", system_error());
#else
  FileDescriptor file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (file.value == -1)
    return error(StorageErrorCode::IoError, path, "Could not open file for sync", system_error());
  struct stat info{};
  if (fstat(file.value, &info) != 0)
    return error(StorageErrorCode::IoError, path, "Could not inspect file", system_error());
  if (!S_ISREG(info.st_mode))
    return error(StorageErrorCode::InvalidPath, path, "Expected regular file");
  if (!lifecycle_sync(file.value))
    return error(StorageErrorCode::DurabilityFailure, path, "Could not sync file", system_error());
#endif
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
remove_file_durable(const fs::path& path) {
  auto valid = validate_lifecycle_file_path(path);
  if (auto* problem = std::get_if<StorageError>(&valid)) return *problem;
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    return error(StorageErrorCode::IoError, path, "Could not inspect removal target", ec);
  const bool exists = fs::exists(status);
  if (exists && (!fs::is_regular_file(status) || fs::is_symlink(status)))
    return error(StorageErrorCode::InvalidPath, path, "Removal target must be a regular file");
#ifdef _WIN32
  if (exists && !DeleteFileW(path.c_str()))
    return error(StorageErrorCode::IoError, path, "Could not remove file", system_error());
#else
  FileDescriptor directory{open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.value == -1)
    return error(StorageErrorCode::IoError, path, "Could not open removal parent", system_error());
  if (exists && unlinkat(directory.value, path.filename().c_str(), 0) != 0)
    return error(StorageErrorCode::IoError, path, "Could not remove file", system_error());
  if (!lifecycle_sync(directory.value))
    return error(StorageErrorCode::DurabilityFailure, path,
                 "File removed but parent sync failed", system_error(), exists);
#endif
  return std::monostate{};
}

}  // namespace folio::core::storage
