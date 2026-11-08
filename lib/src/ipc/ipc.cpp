#include <folio/ipc/ipc.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <climits>
#include <limits>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#endif

namespace folio::application::internal {
namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
core::Result<std::wstring, IpcError> endpoint_user_sid() {
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return IpcError{IpcErrorCode::AccessDenied, "Cannot inspect user token"};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<char> buffer(size);
  const bool success = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!success) return IpcError{IpcErrorCode::AccessDenied, "Cannot inspect user SID"};
  LPWSTR value = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &value))
    return IpcError{IpcErrorCode::IoError, "Cannot encode user SID"};
  std::wstring result(value);
  LocalFree(value);
  return result;
}
#endif

core::Result<fs::path, IpcError>
backend_endpoint_path(const config::ApplicationPaths& application_paths) {
#ifndef _WIN32
  // A private durable-state subdirectory is a safe fallback when no OS runtime
  // directory exists. It holds only a removable socket, never the ownership lock.
  return application_paths.runtime.value_or(application_paths.state / "ipc") / "backend.sock";
#else
  auto sid = endpoint_user_sid();
  if (auto* problem = std::get_if<IpcError>(&sid)) return *problem;
  // One per-user endpoint; the DACL, not its name, controls access.
  return fs::path(L"\\\\.\\pipe\\folio-" + std::get<std::wstring>(sid));
#endif
}

} // namespace

core::Result<ConnectionSettings, IpcError> connection_settings(const config::StoragePaths& paths) {
  auto endpoint = backend_endpoint_path(paths.application);
  if (auto* e = std::get_if<IpcError>(&endpoint)) return *e;
  return ConnectionSettings{std::get<std::filesystem::path>(endpoint)};
}

namespace {
IpcError error(IpcErrorCode code, std::string message) {
  return {code, std::move(message)};
}

IpcError os_error(std::string message) {
#ifndef _WIN32
  const auto value = errno;
  return {value == EACCES || value == EPERM ? IpcErrorCode::AccessDenied : IpcErrorCode::IoError,
          std::move(message),
          {value, std::generic_category()}};
#else
  const auto value = GetLastError();
  return {value == ERROR_ACCESS_DENIED ? IpcErrorCode::AccessDenied : IpcErrorCode::IoError,
          std::move(message),
          {static_cast<int>(value), std::system_category()}};
#endif
}

// Each wait owns its wake resource. stop_callback lifetime ends before it is
// closed, so another thread can safely interrupt a blocked wait without closing
// a stream/listener handle from under its owning thread.
#ifndef _WIN32
core::Result<bool, IpcError> wait_native(int handle, short events,
                                         std::chrono::steady_clock::time_point deadline,
                                         std::stop_token wake) {
  int signal[2];
  if (pipe(signal) != 0) return os_error("Cannot create IPC wake pipe");
  struct Pipe {
    int* handles;
    ~Pipe() {
      close(handles[0]);
      close(handles[1]);
    }
  } cleanup{signal};
  for (auto fd : signal) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
      return os_error("Cannot configure IPC wake pipe");
  }
  std::stop_callback notify(wake, [&] {
    const char byte = 1;
    // One callback per wait; one byte cannot fill the pipe.
    while (::write(signal[1], &byte, 1) < 0 && errno == EINTR) {
    }
  });
  for (;;) {
    if (wake.stop_requested()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return error(IpcErrorCode::Timeout, "IPC wait timed out");
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    const auto timeout = deadline == std::chrono::steady_clock::time_point::max()
                             ? -1
                             : static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX));
    pollfd descriptors[]{{handle, events, 0}, {signal[0], POLLIN, 0}};
    const auto result = poll(descriptors, 2, timeout);
    if (result < 0) {
      if (errno == EINTR) continue;
      return os_error("Cannot wait for IPC readiness");
    }
    if (wake.stop_requested()) return false;
    if (result == 0) continue; // The absolute deadline is checked above.
    if (descriptors[0].revents & POLLNVAL)
      return error(IpcErrorCode::ConnectionLost, "IPC handle is closed");
    // HUP/ERR wake the normal read/write path, which reports the actual failure.
    if (descriptors[0].revents) return true;
  }
}
#else
core::Result<bool, IpcError>
wait_native(HANDLE event, std::chrono::steady_clock::time_point deadline, std::stop_token wake) {
  HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!signal) return os_error("Cannot create IPC wake event");
  struct Event {
    HANDLE handle;
    ~Event() { CloseHandle(handle); }
  } cleanup{signal};
  std::stop_callback notify(wake, [&] { SetEvent(signal); });
  for (;;) {
    if (wake.stop_requested()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return error(IpcErrorCode::Timeout, "IPC wait timed out");
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    const DWORD timeout = deadline == std::chrono::steady_clock::time_point::max()
                              ? INFINITE
                              : static_cast<DWORD>(std::min<std::int64_t>(remaining, INFINITE - 1));
    HANDLE events[]{signal, event};
    const auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (result == WAIT_OBJECT_0) return false;
    if (result == WAIT_OBJECT_0 + 1) return true;
    if (result != WAIT_TIMEOUT) return os_error("Cannot wait for IPC readiness");
  }
}
#endif

void append_number(std::string& out, std::uint32_t value, unsigned count) {
  for (unsigned n = count; n; --n)
    out.push_back(static_cast<char>(value >> ((n - 1) * 8)));
}

std::uint32_t number(std::string_view in) {
  std::uint32_t result = 0;
  for (unsigned char byte : in)
    result = (result << 8) | byte;
  return result;
}
#ifndef _WIN32
core::Result<std::monostate, IpcError> private_parent(const fs::path& parent, bool create) {
  // Reject symlinks and writable ancestors except root-owned sticky directories
  // such as /tmp. Canonical macOS temporary paths are supplied by the resolver.
  auto current = parent.root_path();
  for (const auto& part : parent.relative_path()) {
    current /= part;
    struct stat info{};
    if (lstat(current.c_str(), &info) != 0) {
      if (errno != ENOENT || !create) {
        if (errno == ENOENT) return error(IpcErrorCode::Unavailable, "IPC directory is absent");
        return os_error("Cannot inspect IPC directory");
      }
      if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
        return os_error("Cannot create IPC directory");
      if (lstat(current.c_str(), &info) != 0) return os_error("Cannot inspect IPC directory");
    }
    if (!S_ISDIR(info.st_mode) || (info.st_uid != geteuid() && info.st_uid != 0) ||
        ((info.st_mode & 0022) && !(info.st_uid == 0 && (info.st_mode & S_ISVTX))))
      return error(IpcErrorCode::AccessDenied, "Unsafe IPC directory ancestry");
    if (current == parent && (info.st_uid != geteuid() || (info.st_mode & 0077)))
      return error(IpcErrorCode::AccessDenied,
                   "IPC parent must be owned by this user with mode 0700");
  }
  return std::monostate{};
}

bool same_user(int fd) {
#if defined(__linux__)
  struct ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
         credentials.uid == geteuid();
#elif defined(__APPLE__)
  uid_t user;
  gid_t group;
  return getpeereid(fd, &user, &group) == 0 && user == geteuid();
#endif
}

bool configure(int fd) {
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 || fcntl(fd, F_SETFL, O_NONBLOCK) == -1) return false;
#ifdef __APPLE__
  int enabled = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
  return true;
}

core::Result<sockaddr_un, IpcError> address(const fs::path& endpoint) {
  sockaddr_un result{};
  result.sun_family = AF_UNIX;
  auto text = endpoint.string();
  if (!endpoint.is_absolute() || text.size() >= sizeof(result.sun_path))
    return error(IpcErrorCode::IoError,
                 "IPC socket path is not absolute or exceeds the OS length limit");
  std::memcpy(result.sun_path, text.c_str(), text.size() + 1);
  return result;
}
#else
core::Result<std::wstring, IpcError> user_sid() {
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return os_error("Cannot inspect user token");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<char> buffer(size);
  bool success = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!success) return os_error("Cannot inspect user SID");
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
    return os_error("Cannot encode user SID");
  std::wstring result(text);
  LocalFree(text);
  return result;
}
#endif
} // namespace

std::vector<std::byte> payload_bytes(std::string_view value) {
  auto bytes = std::as_bytes(std::span(value.data(), value.size()));
  return {bytes.begin(), bytes.end()};
}

std::string payload_text(std::span<const std::byte> value) {
  if (value.empty()) return {};
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string encode_message(const Message& message) {
  std::string result;
  append_number(result, static_cast<std::uint32_t>(message.payload.size() + 4), 4);
  append_number(result, message.version, 2);
  append_number(result, static_cast<std::uint16_t>(message.kind), 2);
  if (!message.payload.empty())
    result.append(reinterpret_cast<const char*>(message.payload.data()), message.payload.size());
  return result;
}

core::Result<std::optional<Message>, IpcError> decode_message(std::string& buffer) {
  if (buffer.size() < 4) return std::optional<Message>{};
  const auto size = number(std::string_view(buffer).substr(0, 4));
  if (size < 4 || size > max_message_bytes)
    return error(IpcErrorCode::Protocol, "Invalid IPC frame length");
  if (buffer.size() < 8) return std::optional<Message>{};
  const auto version = number(std::string_view(buffer).substr(4, 2));
  if (version != protocol_version)
    return error(IpcErrorCode::Incompatible, "Unsupported IPC protocol version");
  const auto kind = number(std::string_view(buffer).substr(6, 2));
  switch (static_cast<MessageKind>(kind)) {
  case MessageKind::Hello:
  case MessageKind::Ready:
  case MessageKind::Ping:
  case MessageKind::Pong:
  case MessageKind::Reject:
  case MessageKind::Request:
  case MessageKind::Response:
  case MessageKind::Accepted:
  case MessageKind::Rejected:
    break;
  default:
    return error(IpcErrorCode::Protocol, "Unknown IPC message kind");
  }
  if (buffer.size() < size + 4) return std::optional<Message>{};
  Message result{static_cast<MessageKind>(kind),
                 payload_bytes(std::string_view(buffer).substr(8, size - 4))};
  buffer.erase(0, size + 4);
  return std::optional<Message>{std::move(result)};
}

struct IpcStream::Native {
#ifndef _WIN32
  int handle = -1;

  ~Native() {
    if (handle >= 0) close(handle);
  }
#else
  HANDLE handle = INVALID_HANDLE_VALUE;
  OVERLAPPED reading{}, writing{};
  bool read_pending = false, write_pending = false;
  std::array<std::byte, 65536> input{};
  std::size_t input_size = 0, input_offset = 0;
  std::vector<std::byte> output;

  Native() {
    reading.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (reading.hEvent) writing.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~Native() {
    if (handle != INVALID_HANDLE_VALUE) {
      CancelIoEx(handle, nullptr);
      DWORD count;
      if (read_pending) GetOverlappedResult(handle, &reading, &count, TRUE);
      if (write_pending) GetOverlappedResult(handle, &writing, &count, TRUE);
      CloseHandle(handle);
    }
    if (reading.hEvent) CloseHandle(reading.hEvent);
    if (writing.hEvent) CloseHandle(writing.hEvent);
  }
#endif
};

IpcStream::IpcStream() = default;

IpcStream::IpcStream(std::unique_ptr<Native> native) : native_(std::move(native)) {}

IpcStream::~IpcStream() = default;
IpcStream::IpcStream(IpcStream&&) noexcept = default;
IpcStream& IpcStream::operator=(IpcStream&&) noexcept = default;

core::Result<IpcStream, IpcError> IpcStream::connect(const fs::path& endpoint) {
  auto native = std::make_unique<Native>();
#ifndef _WIN32
  auto safe = private_parent(endpoint.parent_path(), false);
  if (auto* problem = std::get_if<IpcError>(&safe)) return *problem;
  auto addr = address(endpoint);
  if (auto* problem = std::get_if<IpcError>(&addr)) return *problem;
  struct stat info{};
  if (lstat(endpoint.c_str(), &info) != 0) {
    if (errno == ENOENT) return error(IpcErrorCode::Unavailable, "Backend endpoint is absent");
    return os_error("Cannot inspect endpoint");
  }
  if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid())
    return error(IpcErrorCode::AccessDenied, "Unsafe Backend socket");
  // bind() publishes the pathname before chmod()/listen(). Never connect while
  // permissions are unsafe, but allow bounded startup retries through this
  // initialization window. The enclosing directory already passed 0700 checks.
  if (info.st_mode & 0077)
    return error(IpcErrorCode::Unavailable, "Backend socket is not ready with private permissions");
  native->handle = socket(AF_UNIX, SOCK_STREAM, 0);
  if (native->handle < 0 || !configure(native->handle)) return os_error("Cannot create IPC socket");
  if (::connect(native->handle, reinterpret_cast<sockaddr*>(&std::get<sockaddr_un>(addr)),
                sizeof(sockaddr_un)) != 0) {
    if (errno == ECONNREFUSED || errno == ENOENT || errno == EAGAIN || errno == EINPROGRESS)
      return error(IpcErrorCode::Unavailable, "Backend is not accepting connections");
    return os_error("Cannot connect to Backend");
  }
  if (!same_user(native->handle))
    return error(IpcErrorCode::AccessDenied, "Backend peer belongs to another user");
#else
  if (!native->reading.hEvent || !native->writing.hEvent)
    return os_error("Cannot create IPC stream events");
  native->handle =
      CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                  SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | FILE_FLAG_OVERLAPPED, nullptr);
  if (native->handle == INVALID_HANDLE_VALUE) {
    auto code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PIPE_BUSY)
      return error(IpcErrorCode::Unavailable, "Backend pipe is unavailable");
    return os_error("Cannot connect to Backend pipe");
  }
  DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  if (!SetNamedPipeHandleState(native->handle, &mode, nullptr, nullptr))
    return os_error("Cannot configure pipe");
#endif
  return IpcStream(std::move(native));
}

core::Result<std::size_t, IpcError> IpcStream::read(std::span<std::byte> bytes) {
  if (!native_) return error(IpcErrorCode::ConnectionLost, "Connection is closed");
#ifndef _WIN32
  auto count = recv(native_->handle, bytes.data(), bytes.size(), 0);
  if (count > 0) return static_cast<std::size_t>(count);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return std::size_t{0};
#else
  DWORD count = 0;
  if (native_->input_offset == native_->input_size) {
    if (!native_->read_pending) {
      ResetEvent(native_->reading.hEvent);
      if (ReadFile(native_->handle, native_->input.data(),
                   static_cast<DWORD>(native_->input.size()), &count, &native_->reading)) {
        native_->input_size = count;
        native_->input_offset = 0;
      } else if (GetLastError() == ERROR_IO_PENDING) {
        native_->read_pending = true;
        return std::size_t{0};
      } else {
        return error(IpcErrorCode::ConnectionLost, "IPC peer disconnected");
      }
    } else {
      if (!GetOverlappedResult(native_->handle, &native_->reading, &count, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return std::size_t{0};
        return error(IpcErrorCode::ConnectionLost, "IPC peer disconnected");
      }
      native_->read_pending = false;
      native_->input_size = count;
      native_->input_offset = 0;
    }
  }
  const auto copied = std::min(bytes.size(), native_->input_size - native_->input_offset);
  if (copied) {
    std::copy_n(native_->input.data() + native_->input_offset, copied, bytes.data());
    native_->input_offset += copied;
    return copied;
  }
#endif
  return error(IpcErrorCode::ConnectionLost, "IPC peer disconnected");
}

core::Result<std::size_t, IpcError> IpcStream::write(std::span<const std::byte> bytes) {
  if (!native_) return error(IpcErrorCode::ConnectionLost, "Connection is closed");
#ifndef _WIN32
#ifdef __linux__
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  auto count = send(native_->handle, bytes.data(), bytes.size(), flags);
  if (count >= 0) return static_cast<std::size_t>(count);
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return std::size_t{0};
#else
  DWORD count = 0;
  if (!native_->write_pending) {
    native_->output.assign(bytes.begin(), bytes.end());
    ResetEvent(native_->writing.hEvent);
    if (WriteFile(native_->handle, native_->output.data(),
                  static_cast<DWORD>(native_->output.size()), &count, &native_->writing)) {
      native_->output.clear();
      return static_cast<std::size_t>(count);
    }
    if (GetLastError() == ERROR_IO_PENDING) {
      native_->write_pending = true;
      return std::size_t{0};
    }
  } else {
    if (GetOverlappedResult(native_->handle, &native_->writing, &count, FALSE)) {
      native_->write_pending = false;
      native_->output.clear();
      return static_cast<std::size_t>(count);
    }
    if (GetLastError() == ERROR_IO_INCOMPLETE) return std::size_t{0};
  }
#endif
  return error(IpcErrorCode::ConnectionLost, "Cannot write to disconnected IPC peer");
}

core::Result<bool, IpcError> IpcStream::wait(bool writing,
                                             std::chrono::steady_clock::time_point deadline,
                                             std::stop_token wake) {
  if (!native_) return error(IpcErrorCode::ConnectionLost, "Connection is closed");
#ifndef _WIN32
  return wait_native(native_->handle, writing ? POLLOUT : POLLIN, deadline, wake);
#else
  if (!writing && native_->input_offset < native_->input_size) return true;
  if (!writing && !native_->read_pending) {
    // Start the overlapped read without consuming the byte. The next read()
    // returns it; this also detects premature requests while a job is pending.
    ResetEvent(native_->reading.hEvent);
    DWORD count;
    if (ReadFile(native_->handle, native_->input.data(), static_cast<DWORD>(native_->input.size()),
                 &count, &native_->reading)) {
      native_->input_size = count;
      native_->input_offset = 0;
      return true;
    }
    if (GetLastError() != ERROR_IO_PENDING)
      return error(IpcErrorCode::ConnectionLost, "IPC peer disconnected");
    native_->read_pending = true;
  }
  if (writing && !native_->write_pending) return true;
  return wait_native(writing ? native_->writing.hEvent : native_->reading.hEvent, deadline, wake);
#endif
}

bool IpcStream::connected() const {
  if (!native_) return false;
#ifndef _WIN32
  char byte;
  auto count = recv(native_->handle, &byte, 1, MSG_PEEK);
  return count > 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
#else
  DWORD available;
  return PeekNamedPipe(native_->handle, nullptr, 0, nullptr, &available, nullptr) != 0;
#endif
}

struct IpcListener::Native {
  fs::path endpoint;
#ifndef _WIN32
  int handle = -1;
  dev_t device{};
  ino_t inode{};
  bool bound = false;

  ~Native() {
    if (handle >= 0) close(handle);
    struct stat current{};
    if (bound && lstat(endpoint.c_str(), &current) == 0 && current.st_dev == device &&
        current.st_ino == inode)
      unlink(endpoint.c_str());
  }
#else
  HANDLE pending = INVALID_HANDLE_VALUE;
  PSECURITY_DESCRIPTOR security = nullptr;
  OVERLAPPED connecting{};
  bool connect_pending = false;
  bool connected = false;

  Native() { connecting.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

  ~Native() {
    if (pending != INVALID_HANDLE_VALUE) {
      CancelIoEx(pending, nullptr);
      DWORD count;
      if (connect_pending) GetOverlappedResult(pending, &connecting, &count, TRUE);
      CloseHandle(pending);
    }
    if (connecting.hEvent) CloseHandle(connecting.hEvent);
    if (security) LocalFree(security);
  }

  bool make_pipe(bool first) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
    pending = CreateNamedPipeW(
        endpoint.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 65, 65536,
        65536, 0, &attributes);
    return pending != INVALID_HANDLE_VALUE;
  }
#endif
};

IpcListener::IpcListener(std::unique_ptr<Native> native) : native_(std::move(native)) {}

IpcListener::~IpcListener() = default;
IpcListener::IpcListener(IpcListener&&) noexcept = default;
IpcListener& IpcListener::operator=(IpcListener&&) noexcept = default;

core::Result<IpcListener, IpcError> IpcListener::listen(const fs::path& endpoint) {
  auto native = std::make_unique<Native>();
  native->endpoint = endpoint;
#ifndef _WIN32
  auto safe = private_parent(endpoint.parent_path(), true);
  if (auto* problem = std::get_if<IpcError>(&safe)) return *problem;
  auto addr = address(endpoint);
  if (auto* problem = std::get_if<IpcError>(&addr)) return *problem;
  struct stat info{};
  if (lstat(endpoint.c_str(), &info) == 0) {
    if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid())
      return error(IpcErrorCode::AccessDenied, "Unsafe existing IPC endpoint");
    auto probe = IpcStream::connect(endpoint);
    if (std::holds_alternative<IpcStream>(probe))
      return error(IpcErrorCode::Unavailable, "A live Backend endpoint already exists");
    if (std::get<IpcError>(probe).code != IpcErrorCode::Unavailable)
      return std::get<IpcError>(probe);
    if (unlink(endpoint.c_str()) != 0) return os_error("Cannot remove stale IPC endpoint");
  } else if (errno != ENOENT)
    return os_error("Cannot inspect IPC endpoint");
  native->handle = socket(AF_UNIX, SOCK_STREAM, 0);
  if (native->handle < 0 || !configure(native->handle))
    return os_error("Cannot create IPC listener");
  if (bind(native->handle, reinterpret_cast<sockaddr*>(&std::get<sockaddr_un>(addr)),
           sizeof(sockaddr_un)) != 0)
    return os_error("Cannot bind IPC listener");
  if (lstat(endpoint.c_str(), &info) != 0) return os_error("Cannot inspect bound socket");
  native->device = info.st_dev;
  native->inode = info.st_ino;
  native->bound = true;
  if (chmod(endpoint.c_str(), 0600) != 0 || ::listen(native->handle, 64) != 0)
    return os_error("Cannot activate IPC listener");
#else
  if (!native->connecting.hEvent) return os_error("Cannot create IPC listener event");
  auto sid = user_sid();
  if (auto* problem = std::get_if<IpcError>(&sid)) return *problem;
  auto descriptor = L"D:P(A;;GA;;;" + std::get<std::wstring>(sid) + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1,
                                                            &native->security, nullptr))
    return os_error("Cannot secure IPC pipe");
  if (!native->make_pipe(true)) return os_error("Cannot create IPC listener");
#endif
  return IpcListener(std::move(native));
}

core::Result<std::optional<IpcStream>, IpcError> IpcListener::accept() {
  auto stream = std::make_unique<IpcStream::Native>();
#ifndef _WIN32
  stream->handle = ::accept(native_->handle, nullptr, nullptr);
  if (stream->handle < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return std::optional<IpcStream>{};
    return os_error("Cannot accept IPC client");
  }
  if (!configure(stream->handle) || !same_user(stream->handle))
    return std::optional<IpcStream>{}; // Reject unauthenticated peer without stopping
                                       // service.
#else
  if (!stream->reading.hEvent || !stream->writing.hEvent)
    return os_error("Cannot create IPC stream events");
  auto abandoned = [&] {
    CloseHandle(native_->pending);
    native_->pending = INVALID_HANDLE_VALUE;
    native_->connect_pending = false;
    native_->connected = false;
    // Let the owning loop recreate/arm a fresh pipe before its next wait.
    SetEvent(native_->connecting.hEvent);
  };
  if (native_->pending == INVALID_HANDLE_VALUE && !native_->make_pipe(false))
    return os_error("Cannot create pipe instance");
  if (!native_->connected) {
    if (!native_->connect_pending) {
      ResetEvent(native_->connecting.hEvent);
      if (ConnectNamedPipe(native_->pending, &native_->connecting)) {
        native_->connected = true;
      } else {
        const auto code = GetLastError();
        if (code == ERROR_PIPE_CONNECTED)
          native_->connected = true;
        else if (code == ERROR_IO_PENDING)
          native_->connect_pending = true;
        else if (code == ERROR_NO_DATA || code == ERROR_BROKEN_PIPE ||
                 code == ERROR_PIPE_NOT_CONNECTED) {
          abandoned();
          return std::optional<IpcStream>{};
        } else
          return os_error("Cannot accept pipe connection");
      }
    } else {
      DWORD count;
      if (!GetOverlappedResult(native_->pending, &native_->connecting, &count, FALSE)) {
        const auto code = GetLastError();
        if (code == ERROR_IO_INCOMPLETE) return std::optional<IpcStream>{};
        if (code == ERROR_NO_DATA || code == ERROR_BROKEN_PIPE ||
            code == ERROR_PIPE_NOT_CONNECTED) {
          abandoned();
          return std::optional<IpcStream>{};
        }
        return os_error("Cannot accept pipe connection");
      }
      native_->connect_pending = false;
      native_->connected = true;
    }
  }
  if (!native_->connected) return std::optional<IpcStream>{};
  native_->connected = false;
  stream->handle = native_->pending;
  native_->pending = INVALID_HANDLE_VALUE;
#endif
  return std::optional<IpcStream>{IpcStream(std::move(stream))};
}
core::Result<bool, IpcError> IpcListener::wait(std::chrono::steady_clock::time_point deadline,
                                               std::stop_token wake) {
#ifndef _WIN32
  return wait_native(native_->handle, POLLIN, deadline, wake);
#else
  // accept() arms ConnectNamedPipe before the owning loop waits.
  if (native_->connected) return true;
  return wait_native(native_->connecting.hEvent, deadline, wake);
#endif
}

} // namespace folio::application::internal
