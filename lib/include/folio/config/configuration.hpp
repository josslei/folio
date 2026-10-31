#pragma once

#include <folio/core/result.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace folio::config {

struct ApplicationPaths {
  std::filesystem::path config;
  std::filesystem::path data;
  std::filesystem::path state;
  std::filesystem::path cache;
  // Platform-specific diagnostics location; resolution does not create it.
  std::filesystem::path logs;
  std::optional<std::filesystem::path> runtime;
};

struct StoragePaths {
  // User-configurable authoritative locations, independent of application paths.
  std::filesystem::path normal_root;
  std::filesystem::path vault_root;
  ApplicationPaths application;
};

enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical };

// Validated application settings for one startup, independent of the TOML layout.
// Contains effective paths and diagnostic preferences; future supported settings
// belong here too.
// Consumers receive the settings they need (for example, paths for Storage).
struct EffectiveConfig {
  StoragePaths paths;
  LogLevel log_level;
};

enum class ConfigurationErrorCode { SyntaxError, InvalidValue, InvalidPath, IoError };

struct ConfigurationError {
  ConfigurationErrorCode code;
  std::filesystem::path file;
  std::string message;
  std::error_code system_error;
  bool publication_completed = false;
};

// Validate effective root layout and existing path properties without creating files.
core::Result<StoragePaths, ConfigurationError> validate_effective_paths(
    const StoragePaths& paths);

// The single source of built-in settings; validation occurs during loading.
core::Result<EffectiveConfig, ConfigurationError> configuration_defaults(const ApplicationPaths& application);
core::Result<EffectiveConfig, ConfigurationError> load_configuration(
    const ApplicationPaths& application,
    const std::filesystem::path& file);
core::Result<EffectiveConfig, ConfigurationError> load_configuration();

// Pure platform resolution; creates nothing.
core::Result<ApplicationPaths, ConfigurationError> application_paths();

}  // namespace folio::config
