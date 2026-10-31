#include "application/internal/configuration.hpp"
#include "application/internal/configuration_test_hooks.hpp"
#include <toml++/toml.hpp>
#include <cstdlib>
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#endif

namespace folio::application::internal {
namespace fs = std::filesystem;
namespace storage = core::storage;

namespace {
ProcessError invalid(std::string message) {
  return {ProcessErrorCode::InvalidArgument, std::move(message), {}};
}
#ifndef _WIN32
fs::path environment_path(const char* key) {
  const char* value = std::getenv(key);
  return value && fs::path(value).is_absolute() ? fs::path(value) : fs::path{};
}
#endif
}  // namespace

core::Result<storage::ApplicationPaths, ProcessError> application_paths() {
#if defined(__linux__) || defined(__APPLE__)
  auto home = environment_path("HOME");
  if (home.empty()) {
    auto* account = getpwuid(geteuid());
    if (account)
      home = account->pw_dir;
  }
  if (!home.is_absolute())
    return invalid("Cannot resolve current user's home directory");
#if defined(__linux__)
  auto directory = [&](const char* key, const fs::path& fallback) {
    auto root = environment_path(key);
    return (root.empty() ? fallback : root) / "folio";
  };
  std::optional<fs::path> runtime;
  if (auto root = environment_path("XDG_RUNTIME_DIR"); !root.empty())
    runtime = root / "folio";
  return storage::ApplicationPaths{directory("XDG_CONFIG_HOME", home / ".config"),
                                   directory("XDG_DATA_HOME", home / ".local/share"),
                                   directory("XDG_STATE_HOME", home / ".local/state"),
                                   directory("XDG_CACHE_HOME", home / ".cache"),
                                   runtime};
#elif defined(__APPLE__)
  auto support = home / "Library/Application Support/Folio";
  std::optional<fs::path> runtime;
  const auto size = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
  if (size) {
    std::string buffer(size, '\0');
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), size) == size) {
      std::error_code ec;
      auto root = fs::canonical(fs::path(buffer.c_str()), ec);
      if (!ec)
        runtime = root / "folio";
    }
  }
  return storage::ApplicationPaths{support / "config", support / "data",
                                   support / "state", home / "Library/Caches/Folio",
                                   runtime};
#endif
#elif defined(_WIN32)
  auto known = [](REFKNOWNFOLDERID id) -> fs::path {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value)))
      return {};
    fs::path result(value);
    CoTaskMemFree(value);
    return result;
  };
  auto roaming = known(FOLDERID_RoamingAppData), local = known(FOLDERID_LocalAppData);
  if (roaming.empty() || local.empty())
    return invalid("Cannot resolve user Known Folders");
  return storage::ApplicationPaths{roaming / L"Folio/config", local / L"Folio/data",
                                   local / L"Folio/state", local / L"Folio/cache",
                                   std::nullopt};
#endif
}

core::Result<storage::StoragePaths, ProcessError> load_configuration(
    const storage::ApplicationPaths& application, const fs::path& file) {
  if (!file.is_absolute())
    return invalid("Configuration file must be absolute");
  auto bytes = storage::read_binary_file(file, 64 * 1024);
  if (auto* error = std::get_if<storage::BinaryReadError>(&bytes))
    return ProcessError{
        ProcessErrorCode::IoError,
        "Cannot read Folio configuration: " + file.string() + ": " + error->message,
        error->system_error};
  try {
    auto table = toml::parse(std::get<std::string>(bytes));
    if (table.size() != 2)
      return invalid("Configuration requires only normal_root and vault_root");
    auto normal = table["normal_root"].value<std::string>();
    auto vault = table["vault_root"].value<std::string>();
    if (!normal || !vault)
      return invalid("Configuration requires string normal_root and vault_root");
    storage::StoragePaths paths{fs::u8path(*normal), fs::u8path(*vault), application};
    auto valid = storage::validate_storage_paths(paths);
    if (auto* error = std::get_if<storage::StorageError>(&valid))
      return invalid(error->message);
    return std::get<storage::StoragePaths>(std::move(valid));
  } catch (const std::exception& e) {
    return invalid(std::string("Invalid Folio configuration: ") + e.what());
  }
}

core::Result<storage::StoragePaths, ProcessError> load_configuration() {
  if (testing::configuration_override) {
    return *testing::configuration_override;
  }
  auto resolved = application_paths();
  if (auto* error = std::get_if<ProcessError>(&resolved))
    return *error;
  auto paths = std::get<storage::ApplicationPaths>(std::move(resolved));
  auto file = paths.config / "config.toml";
  // Explicit bootstrap override, also useful for isolated executable tests.
#ifdef _WIN32
  if (const auto* override = _wgetenv(L"FOLIO_CONFIG_FILE"))
    file = override;
#else
  if (const auto* override = std::getenv("FOLIO_CONFIG_FILE"))
    file = override;
#endif
  return load_configuration(paths, file);
}
}  // namespace folio::application::internal

namespace folio::application::internal {
#ifdef _WIN32
namespace {
core::Result<std::wstring, IpcError> endpoint_user_sid() {
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return IpcError{IpcErrorCode::AccessDenied, "Cannot inspect user token"};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<char> buffer(size);
  const bool success =
      GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!success)
    return IpcError{IpcErrorCode::AccessDenied, "Cannot inspect user SID"};
  LPWSTR value = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                              &value))
    return IpcError{IpcErrorCode::IoError, "Cannot encode user SID"};
  std::wstring result(value);
  LocalFree(value);
  return result;
}
}  // namespace
#endif

core::Result<fs::path, IpcError> ipc_endpoint(
    const core::storage::StoragePaths& paths) {
#ifndef _WIN32
  // A private durable-state subdirectory is a safe fallback when no OS runtime
  // directory exists. It holds only a removable socket, never the ownership lock.
  return paths.application.runtime.value_or(paths.application.state / "ipc") /
         "backend.sock";
#else
  auto sid = endpoint_user_sid();
  if (auto* problem = std::get_if<IpcError>(&sid))
    return *problem;
  // One per-user endpoint; the DACL, not its name, controls access.
  return fs::path(L"\\\\.\\pipe\\folio-" + std::get<std::wstring>(sid));
#endif
}

core::Result<ConnectionSettings, IpcError> connection_settings(
    const core::storage::StoragePaths& paths) {
  auto endpoint = ipc_endpoint(paths);
  if (auto* e = std::get_if<IpcError>(&endpoint))
    return *e;
  return ConnectionSettings{std::get<std::filesystem::path>(endpoint)};
}

core::Result<ConnectionSettings, IpcError> connection_settings() {
  auto paths = load_configuration();
  if (auto* e = std::get_if<ProcessError>(&paths))
    return IpcError{IpcErrorCode::IoError, e->message, e->system_error};
  return connection_settings(std::get<core::storage::StoragePaths>(paths));
}

}  // namespace folio::application::internal
