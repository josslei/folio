#include "folio/storage/filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace folio::storage {
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

std::atomic<std::uint64_t> temporary_sequence{0};

std::string temporary_name() {
#ifdef _WIN32
  const auto process = GetCurrentProcessId();
#else
  const auto process = getpid();
#endif
  return ".folio-tmp-" + std::to_string(process) + "-" +
         std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
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

Result<StoragePaths> validate_storage_paths(const StoragePaths& paths) {
  const std::array roots{paths.normal_root, paths.vault_root,
                          paths.state_root, paths.cache_root};
  std::array<fs::path, 4> resolved;
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
  for (std::size_t i = 0; i < resolved.size(); ++i) {
    for (std::size_t j = i + 1; j < resolved.size(); ++j) {
      if (overlaps(resolved[i], resolved[j])) {
        return error(StorageErrorCode::InvalidPath, roots[j],
                     "Storage roots must not overlap");
      }
    }
  }
  return paths;
}

Result<std::string> normal_entry_filename(EntryType type, const core::Id& id,
                                         core::Timestamp created) {
  using namespace std::chrono;
  const char* extension;
  switch (type) {
    case EntryType::Log:
    case EntryType::Memo:
      extension = ".md";
      break;
    case EntryType::Todo:
      extension = ".toml";
      break;
    default:
      return error(StorageErrorCode::InvalidEntryType, {}, "Invalid entry type");
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

Result<std::monostate> write_file_atomic(const fs::path& path, std::string_view contents) {
  if (path.empty() || has_nul(path) || path.filename().empty() ||
      path.filename() == "." || path.filename() == "..") {
    return error(StorageErrorCode::InvalidPath, path, "Expected a file path without NUL bytes");
  }
  const auto parent = path.has_parent_path() ? path.parent_path() : fs::path{"."};
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
      return error(StorageErrorCode::InvalidPath, path, "Destination must be a regular file");
    }
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    return error(StorageErrorCode::IoError, path, "Could not inspect destination", system_error());
  }
  TemporaryFile temporary;
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary.path = parent / temporary_name();
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
  if (!MoveFileExW(temporary.path.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return error(StorageErrorCode::IoError, path, "Could not replace destination", system_error());
  }
  temporary.owned = false;
#else
  FileDescriptor directory{open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (directory.value == -1) {
    return error(StorageErrorCode::IoError, path, "Could not open parent directory", system_error());
  }
  const auto filename = path.filename().native();
  struct stat status{};
  if (fstatat(directory.value, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(status.st_mode)) {
      return error(StorageErrorCode::InvalidPath, path, "Destination must be a regular file");
    }
  } else if (errno != ENOENT) {
    return error(StorageErrorCode::IoError, path, "Could not inspect destination", system_error());
  }
  TemporaryFile temporary{directory.value};
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary.name = temporary_name();
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
  if (!sync_file(temporary.descriptor)) {
    return error(StorageErrorCode::IoError, path, "Could not sync temporary file", system_error());
  }
  const auto descriptor = temporary.descriptor;
  temporary.descriptor = -1;
  // Do not retry close after EINTR: the descriptor may already have been released.
  if (close(descriptor) == -1) {
    return error(StorageErrorCode::IoError, path, "Could not close temporary file", system_error());
  }
  if (renameat(directory.value, temporary.name.c_str(), directory.value, filename.c_str()) == -1) {
    return error(StorageErrorCode::IoError, path, "Could not replace destination", system_error());
  }
  temporary.owned = false;
  if (!sync_file(directory.value)) {
    return error(StorageErrorCode::DurabilityFailure, path, "Replacement completed but parent directory sync failed",
                 system_error(), true);
  }
#endif
  return std::monostate{};
}

}  // namespace folio::storage
