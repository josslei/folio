#include <folio/config/configuration.hpp>
#include "io/filesystem.hpp"
#include "io/path.hpp"
#include "config_template.hpp"
#include <thread>
#include <chrono>
#include <vector>
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

namespace folio::config {
namespace fs = std::filesystem;


namespace {
ConfigurationError invalid(std::string message) {
  return {ConfigurationErrorCode::InvalidPath, {}, std::move(message), {}};
}
#ifndef _WIN32
fs::path environment_path(const char* key) {
  const char* value = std::getenv(key);
  return value && fs::path(value).is_absolute() ? fs::path(value) : fs::path{};
}
#endif
}  // namespace

core::Result<ApplicationPaths, ConfigurationError> application_paths() {
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
  return ApplicationPaths{directory("XDG_CONFIG_HOME", home / ".config"),
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
  return ApplicationPaths{support / "config", support / "data",
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
  return ApplicationPaths{roaming / L"Folio/config", local / L"Folio/data",
                                   local / L"Folio/state", local / L"Folio/cache",
                                   std::nullopt};
#endif
}

namespace {
ConfigurationError path_error(ConfigurationErrorCode code, fs::path path,
                              std::string message, std::error_code ec = {}) {
  return {code, std::move(path), std::move(message), ec};
}
}  // namespace

core::Result<StoragePaths, ConfigurationError> validate_effective_paths(const StoragePaths& paths) {
  std::vector<fs::path> roots{paths.normal_root, paths.vault_root,
      paths.application.config, paths.application.data,
      paths.application.state, paths.application.cache};
  if (paths.application.runtime) roots.push_back(*paths.application.runtime);
  std::vector<fs::path> resolved(roots.size());
  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].empty() || !roots[i].is_absolute() || io::has_nul(roots[i])) {
      return path_error(ConfigurationErrorCode::InvalidPath, roots[i],
                   "Storage roots must be nonempty absolute paths without NUL bytes");
    }
    std::error_code ec;
    resolved[i] = fs::weakly_canonical(roots[i], ec);
    if (ec) {
      return path_error(ConfigurationErrorCode::IoError, roots[i], "Could not resolve storage root", ec);
    }
    if (resolved[i].has_relative_path() && resolved[i].filename().empty()) {
      resolved[i] = resolved[i].parent_path();
    }
    const auto status = fs::status(resolved[i], ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      return path_error(ConfigurationErrorCode::IoError, roots[i], "Could not inspect storage root", ec);
    }
    if (fs::exists(status) && !fs::is_directory(status)) {
      return path_error(ConfigurationErrorCode::InvalidPath, roots[i], "Storage root is not a directory");
    }
  }
  for (std::size_t i = 0; i < 2; ++i) {
    for (std::size_t j = i + 1; j < resolved.size(); ++j) {
      // Data is a namespace for durable application data, including the default
      // authoritative roots. Only a strict Data ancestor is allowed; Data itself
      // and support directories inside authoritative roots remain forbidden.
      if (j == 3 && !io::component_equal(resolved[i], resolved[j]) &&
          io::contains_path(resolved[j], resolved[i])) {
        continue;
      }
      if (io::overlaps(resolved[i], resolved[j])) {
        return path_error(ConfigurationErrorCode::InvalidPath, roots[j],
                     "Authoritative roots conflict with each other or application support directories");
      }
    }
  }
  return paths;
}

namespace {
core::Result<EffectiveConfig, ConfigurationError> parse_configuration(
    std::string_view text, const ApplicationPaths& application, const fs::path& file,
    const toml::table* defaults = nullptr) {
  auto invalid_value = [&](std::string message) -> ConfigurationError {
    return {ConfigurationErrorCode::InvalidValue, file, std::move(message), {}};
  };
  try {
    auto table = toml::parse(text);
    for (const auto& [key, value] : table) {
      if (key != "schema_version" && key != "general")
        return invalid_value("Unknown configuration key: " + std::string(key.str()));
    }
    if (table["schema_version"]) {
      auto version = table["schema_version"].value<int64_t>();
      if (!version || *version != 1) return invalid_value("schema_version must be 1");
    } else if (!defaults) return invalid_value("Factory template requires schema_version");
    auto* general = table["general"].as_table();
    if (table["general"] && !general) return invalid_value("general must be a table");
    if (general) {
      for (const auto& [key, value] : *general)
        if (key != "normal_storage" && key != "vault_storage")
          return invalid_value("Unknown general setting: " + std::string(key.str()));
    }
    EffectiveConfig config{{{}, {}, application}};
    for (auto name : {"normal_storage", "vault_storage"}) {
      const toml::node* node = general ? general->get(name) : nullptr;
      const auto* storage = node ? node->as_table() : nullptr;
      if (node && !storage) return invalid_value(std::string(name) + " must be a table");
      const auto* fallback = defaults ? (*defaults)["general"][name].as_table() : nullptr;
      if (!storage) storage = fallback;
      if (!storage) return invalid_value("Factory template requires storage settings");
      for (const auto& [key, value] : *storage)
        if (key != "use_default_location" && key != "path")
          return invalid_value("Unknown storage setting: " + std::string(key.str()));
      const auto* flag = storage->get("use_default_location");
      if (!flag && fallback) flag = fallback->get("use_default_location");
      auto use_default = flag ? flag->value<bool>() : std::nullopt;
      if (!use_default) return invalid_value("use_default_location must be a boolean");
      const auto* path = storage->get("path");
      fs::path resolved;
      if (*use_default) {
        if (path) return invalid_value("path conflicts with use_default_location = true");
        resolved = application.data / (std::string_view(name) == "normal_storage" ? "normal" : "vault");
      } else {
        auto value = path ? path->value<std::string>() : std::nullopt;
        if (!value) return invalid_value("Custom storage requires a string path");
        resolved = fs::path(std::u8string(value->begin(), value->end()));
      }
      (std::string_view(name) == "normal_storage" ? config.paths.normal_root : config.paths.vault_root) = std::move(resolved);
    }
    auto validated = validate_effective_paths(config.paths);
    if (auto* error = std::get_if<ConfigurationError>(&validated)) {
      auto result = *error;
      result.file = file;
      return result;
    }
    return config;
  } catch (const toml::parse_error& error) {
    return ConfigurationError{ConfigurationErrorCode::SyntaxError, file, std::string(error.description()), {}};
  } catch (const fs::filesystem_error& error) {
    return ConfigurationError{ConfigurationErrorCode::InvalidPath, file, error.what(), error.code()};
  }
}
}  // namespace

core::Result<EffectiveConfig, ConfigurationError> configuration_defaults(
    const ApplicationPaths& application) {
  return parse_configuration(factory_template, application, {});
}

core::Result<EffectiveConfig, ConfigurationError> load_configuration(
    const ApplicationPaths& application, const fs::path& file) {
  auto failure = [&](ConfigurationErrorCode code, std::string message,
                     std::error_code ec = {}) {
    return ConfigurationError{code, file, std::move(message), ec};
  };
  if (!file.is_absolute() ||
      file.native().find(fs::path::value_type{}) != fs::path::string_type::npos) {
    return failure(ConfigurationErrorCode::InvalidPath,
                   "Configuration file must be an absolute path without NUL bytes");
  }
  // Inspect every existing component: dangling symlinks are errors, not absence.
  // Like other startup validation, callers exclude concurrent external replacement.
  auto current = file.root_path();
  bool missing = false;
  for (const auto& component : file.relative_path()) {
    current /= component;
    std::error_code ec;
    const auto status = fs::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found)) {
      missing = true;
      break;
    }
    if (ec) {
      return failure(ConfigurationErrorCode::IoError,
                     "Cannot inspect Folio configuration: " + current.string(), ec);
    }
    if (fs::is_symlink(status)) {
      return failure(ConfigurationErrorCode::InvalidPath,
                     "Configuration path must not contain symlinks: " + current.string());
    }
    if ((current == file && !fs::is_regular_file(status)) ||
        (current != file && !fs::is_directory(status))) {
      return failure(ConfigurationErrorCode::InvalidPath,
                     "Unexpected configuration path type: " + current.string());
    }
  }
  if (missing) {
    auto defaults = configuration_defaults(application);
    if (auto* error = std::get_if<ConfigurationError>(&defaults)) return *error;
    auto created = io::create_directories_durable(file.parent_path());
    if (auto* error = std::get_if<io::FileError>(&created))
      return ConfigurationError{ConfigurationErrorCode::IoError, file, error->message,
                                error->system_error, error->replacement_completed};
    for (int attempt = 0; ; ++attempt) {
      auto result = io::create_file_atomic(file, factory_template);
      auto* error = std::get_if<io::FileError>(&result);
      if (!error) break;
      if (!error->replacement_completed && error->system_error == std::errc::file_exists)
        return load_configuration(application, file);
      if (!error->replacement_completed && attempt < 100 &&
          (error->system_error == std::errc::device_or_resource_busy ||
           error->system_error == std::errc::resource_unavailable_try_again)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      return ConfigurationError{ConfigurationErrorCode::IoError, file, error->message,
                                error->system_error, error->replacement_completed};
    }
  }
  auto bytes = io::read_file(file, 64 * 1024);
  if (auto* error = std::get_if<io::ReadError>(&bytes)) {
    return failure(error->code == io::ReadErrorCode::LimitExceeded
                       ? ConfigurationErrorCode::InvalidValue : ConfigurationErrorCode::IoError,
                   "Cannot read Folio configuration: " + file.string() + ": " + error->message,
                   error->system_error);
  }
  try {
    const auto defaults = toml::parse(factory_template);
    return parse_configuration(std::get<std::string>(bytes), application, file, &defaults);
  } catch (const toml::parse_error& error) {
    return failure(ConfigurationErrorCode::SyntaxError,
                   "Invalid embedded factory configuration: " + std::string(error.description()));
  }
}

core::Result<EffectiveConfig, ConfigurationError> load_configuration() {
  auto resolved = application_paths();
  if (auto* error = std::get_if<ConfigurationError>(&resolved)) {
    return ConfigurationError{ConfigurationErrorCode::InvalidPath, {},
                              error->message, error->system_error};
  }
  auto paths = std::get<ApplicationPaths>(std::move(resolved));
  auto file = paths.config / "folio_config.toml";
  // Explicit bootstrap override; a missing selected file is created from the template.
#ifdef _WIN32
  if (const auto* override = _wgetenv(L"FOLIO_CONFIG_FILE"))
    file = override;
#else
  if (const auto* override = std::getenv("FOLIO_CONFIG_FILE"))
    file = override;
#endif
  return load_configuration(paths, file);
}
}  // namespace folio::config
