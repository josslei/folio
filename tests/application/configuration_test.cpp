#include <folio/config/configuration.hpp>
#include "runtime/backend_states.hpp"

#include <folio/core/types.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <future>

namespace folio::application::internal {
namespace {
namespace fs = std::filesystem;

class Configuration : public ::testing::Test {
protected:
  fs::path base;
  fs::path file;
  folio::config::ApplicationPaths application;

  void SetUp() override {
    base = fs::temp_directory_path() / ("fcfg-" + core::Id::generate().to_string().substr(0, 8));
    fs::create_directory(base);
    base = fs::canonical(base);
    application = {base / "config", base / "data", base / "state",
                   base / "cache",  base / "logs", base / "runtime"};
    file = application.config / "folio_config.toml";
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(base, ignored);
  }

  void write(std::string_view text) {
    fs::create_directory(application.config);
    std::ofstream(file) << text;
  }

  void expect_error(std::string_view text, folio::config::ConfigurationErrorCode code) {
    write(text);
    auto result = folio::config::load_configuration(application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(result));
    EXPECT_EQ(std::get<folio::config::ConfigurationError>(result).code, code);
    EXPECT_EQ(std::get<folio::config::ConfigurationError>(result).file, file);
  }
};

TEST_F(Configuration, MissingFileCreatesReadableFactoryTemplate) {
  const auto defaults = folio::config::configuration_defaults(application);
  auto result = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(result));
  const auto& config = std::get<folio::config::EffectiveConfig>(result);
  EXPECT_EQ(config.paths.normal_root, application.data / "normal");
  EXPECT_EQ(config.paths.vault_root, application.data / "vault");
  EXPECT_EQ(config.paths.normal_root,
            std::get<folio::config::EffectiveConfig>(defaults).paths.normal_root);
  EXPECT_TRUE(fs::is_regular_file(file));
  EXPECT_FALSE(fs::exists(application.data));
}

TEST_F(Configuration, ConcurrentCreationLoadsSameTemplateWithoutReplacingIt) {
  auto first = std::async(std::launch::async,
                          [&] { return folio::config::load_configuration(application, file); });
  auto second = folio::config::load_configuration(application, file);
  EXPECT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(first.get()));
  EXPECT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(second));
  std::ifstream input(file);
  std::string text((std::istreambuf_iterator<char>(input)), {});
  EXPECT_NE(text.find("schema_version = 1"), std::string::npos);
  EXPECT_NE(text.find("use_default_location = true"), std::string::npos);
}

TEST_F(Configuration, RejectsUnsupportedVersionAndContradictoryPathSelection) {
  expect_error("schema_version = 2", folio::config::ConfigurationErrorCode::InvalidValue);
  expect_error("[general.normal_storage]\nuse_default_location = true\npath = '/custom'",
               folio::config::ConfigurationErrorCode::InvalidValue);
  expect_error("[general.normal_storage]\nuse_default_location = false",
               folio::config::ConfigurationErrorCode::InvalidValue);
  expect_error("[general.normal_storage]\nuse_default_location = 'true'",
               folio::config::ConfigurationErrorCode::InvalidValue);
}

TEST_F(Configuration, LoggingDefaultsOverridesAndValidation) {
  auto defaults = folio::config::configuration_defaults(application);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(defaults));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(defaults).log_level,
            folio::config::LogLevel::Warn);
  // Older files without diagnostics keep the template default, without rewrite.
  write("schema_version = 1\n");
  auto omitted = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(omitted));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(omitted).log_level,
            folio::config::LogLevel::Warn);
  const std::pair<std::string, folio::config::LogLevel> levels[] = {
      {"trace", folio::config::LogLevel::Trace}, {"debug", folio::config::LogLevel::Debug},
      {"info", folio::config::LogLevel::Info},   {"warn", folio::config::LogLevel::Warn},
      {"error", folio::config::LogLevel::Error}, {"critical", folio::config::LogLevel::Critical}};
  for (const auto& [name, level] : levels) {
    write("[diagnostics]\nlog_level = '" + name + "'\n");
    auto loaded = folio::config::load_configuration(application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(loaded));
    EXPECT_EQ(std::get<folio::config::EffectiveConfig>(loaded).log_level, level);
  }
  for (const auto& text : {"[diagnostics]\nlog_level = 'verbose'",
                           "[diagnostics]\nlog_level = 'off'", "[diagnostics]\nlog_level = 2",
                           "diagnostics = false", "[diagnostics]\nlog_levle = 'warn'"}) {
    expect_error(text, folio::config::ConfigurationErrorCode::InvalidValue);
  }
}

TEST_F(Configuration, InvalidExistingFileIsNeverReplacedByFactoryDefaults) {
  const std::string original = "schema_version = 1\n[broken";
  write(original);
  auto result = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(result));
  std::ifstream input(file);
  EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), {}), original);
}

TEST_F(Configuration, Utf8RootOverridePreservesUnicodePath) {
  const auto expected = base / fs::path(u8"笔记-é");
  const auto encoded = expected.generic_u8string();
  write("[general.normal_storage]\nuse_default_location = false\npath = '" +
        std::string(encoded.begin(), encoded.end()) + "'\n");
  auto result = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(result));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(result).paths.normal_root, expected);
  EXPECT_FALSE(fs::exists(expected));
}

TEST_F(Configuration, PartialOverrideRetainsOtherDefaultsAndPreservesExistingData) {
  auto normal = base / "existing-normal";
  fs::create_directory(normal);
  std::ofstream(normal / "sentinel") << "preserve";
  write("[general.normal_storage]\nuse_default_location = false\npath = '" +
        normal.generic_string() + "'\n");
  auto result = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(result));
  const auto& config = std::get<folio::config::EffectiveConfig>(result);
  EXPECT_EQ(config.paths.normal_root, normal);
  EXPECT_EQ(config.paths.vault_root, application.data / "vault");
  EXPECT_TRUE(fs::exists(normal / "sentinel"));
  EXPECT_FALSE(fs::exists(application.data));
}

TEST_F(Configuration, EmptyFileUsesDefaultsAndBothRootsCanBeOverridden) {
  write("");
  auto empty = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(empty));
  write("[general.normal_storage]\nuse_default_location = false\npath = '" +
        (base / "n").generic_string() +
        "'\n[general.vault_storage]\nuse_default_location = false\npath = '" +
        (base / "v").generic_string() + "'\n");
  auto both = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(both));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(both).paths.normal_root, base / "n");
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(both).paths.vault_root, base / "v");
}

TEST_F(Configuration, RejectsSyntaxErrorsTypesAndUnknownKeys) {
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = [",
               folio::config::ConfigurationErrorCode::SyntaxError);
  for (auto value : {"42", "-1", "true", "[]", "{}", "1.5"}) {
    expect_error(std::string("[general.normal_storage]\nuse_default_location = false\npath = ") +
                     value,
                 folio::config::ConfigurationErrorCode::InvalidValue);
  }
  expect_error("normal_rooot = '/somewhere'", folio::config::ConfigurationErrorCode::InvalidValue);
  expect_error("[logging]\nlevel = 'debug'", folio::config::ConfigurationErrorCode::InvalidValue);
}

TEST_F(Configuration, RejectsRelativeEmptyNulAndOverlappingRoots) {
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = 'relative'",
               folio::config::ConfigurationErrorCode::InvalidPath);
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = ''",
               folio::config::ConfigurationErrorCode::InvalidPath);
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = \"/bad\\u0000path\"",
               folio::config::ConfigurationErrorCode::InvalidPath);
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = '" +
                   (application.data / "vault").generic_string() + "'",
               folio::config::ConfigurationErrorCode::InvalidPath);
  expect_error("[general.normal_storage]\nuse_default_location = false\npath = '" +
                   application.state.generic_string() + "'",
               folio::config::ConfigurationErrorCode::InvalidPath);
  EXPECT_FALSE(fs::exists(application.data));
}

TEST_F(Configuration, RejectsUnexpectedFileTypesAndOversizedConfiguration) {
  fs::create_directories(file);
  auto directory = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(directory));
  EXPECT_EQ(std::get<folio::config::ConfigurationError>(directory).code,
            folio::config::ConfigurationErrorCode::InvalidPath);
  fs::remove(file);
  expect_error(std::string(64 * 1024 + 1, ' '),
               folio::config::ConfigurationErrorCode::InvalidValue);
}

TEST_F(Configuration, RejectsSymlinkAndDanglingSymlinkInsteadOfUsingDefaults) {
  fs::create_directory(application.config);
  auto target = base / "target";
  std::error_code ec;
  fs::create_symlink(target, file, ec);
  if (ec) GTEST_SKIP() << "Symlink creation unavailable: " << ec.message();
  for (int i = 0; i < 2; ++i) {
    auto result = folio::config::load_configuration(application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(result));
    EXPECT_EQ(std::get<folio::config::ConfigurationError>(result).code,
              folio::config::ConfigurationErrorCode::InvalidPath);
    std::ofstream(target) << "";
  }
}

#ifndef _WIN32
TEST_F(Configuration, PermissionErrorsAreNotTreatedAsMissingConfiguration) {
  write("");
  fs::permissions(application.config, fs::perms::none);
  auto result = folio::config::load_configuration(application, file);
  fs::permissions(application.config, fs::perms::owner_all);
  if (std::holds_alternative<folio::config::EffectiveConfig>(result)) {
    GTEST_SKIP() << "Execution account bypasses permission restrictions";
  }
  EXPECT_EQ(std::get<folio::config::ConfigurationError>(result).code,
            folio::config::ConfigurationErrorCode::IoError);
}
#endif

TEST_F(Configuration, DefaultsInitializeBackendAndRemainAStartupSnapshot) {
  auto loaded = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(loaded));
  auto config = std::get<folio::config::EffectiveConfig>(loaded);
  BackendContext context;
  context.startup_paths = config.paths;
  ASSERT_EQ(Bootstrap(context), BackendActionOutcome::Success)
      << (context.diagnostics.empty() ? "" : diagnostic_message(context.diagnostics.front()));
  EXPECT_EQ(context.paths->normal_root, application.data / "normal");
  EXPECT_FALSE(fs::exists(config.paths.vault_root));
  write("[general.normal_storage]\nuse_default_location = false\npath = '" +
        (base / "other-normal").generic_string() + "'");
  EXPECT_EQ(context.paths->normal_root, config.paths.normal_root);
  EXPECT_FALSE(fs::exists(base / "other-normal"));
  EXPECT_EQ(Stopping(context), BackendActionOutcome::Success);
  EXPECT_EQ(Finish(context), BackendActionOutcome::Success);
  EXPECT_FALSE(fs::exists(application.state / "session.marker"));
}

#ifndef _WIN32
class EnvironmentOverride {
public:
  EnvironmentOverride(const char* name, const std::optional<std::string>& value) : name_(name) {
    if (const auto* previous = std::getenv(name)) previous_ = previous;
    if (value)
      setenv(name, value->c_str(), 1);
    else
      unsetenv(name);
  }
  ~EnvironmentOverride() {
    if (previous_)
      setenv(name_, previous_->c_str(), 1);
    else
      unsetenv(name_);
  }

private:
  const char* name_;
  std::optional<std::string> previous_;
};

TEST_F(Configuration, PlatformDefaultsAreResolvedWithoutCreatingDirectories) {
  EnvironmentOverride home("HOME", base.string());
#if defined(__linux__)
  EnvironmentOverride config("XDG_CONFIG_HOME", std::nullopt);
  EnvironmentOverride data("XDG_DATA_HOME", std::nullopt);
  EnvironmentOverride state("XDG_STATE_HOME", std::nullopt);
  EnvironmentOverride cache("XDG_CACHE_HOME", std::nullopt);
  EnvironmentOverride runtime("XDG_RUNTIME_DIR", std::nullopt);
#endif
  auto result = folio::config::application_paths();
  ASSERT_TRUE(std::holds_alternative<folio::config::ApplicationPaths>(result));
  const auto& paths = std::get<folio::config::ApplicationPaths>(result);
#if defined(__linux__)
  EXPECT_EQ(paths.config, base / ".config/folio");
  EXPECT_EQ(paths.data, base / ".local/share/folio");
  EXPECT_EQ(paths.state, base / ".local/state/folio");
  EXPECT_EQ(paths.logs, base / ".local/state/folio/logs");
  EXPECT_EQ(paths.cache, base / ".cache/folio");
  EXPECT_FALSE(paths.runtime);
#elif defined(__APPLE__)
  EXPECT_EQ(paths.config, base / "Library/Application Support/Folio/config");
  EXPECT_EQ(paths.data, base / "Library/Application Support/Folio/data");
  EXPECT_EQ(paths.state, base / "Library/Application Support/Folio/state");
  EXPECT_EQ(paths.cache, base / "Library/Caches/Folio");
  EXPECT_EQ(paths.logs, base / "Library/Logs/Folio");
#endif
  EXPECT_TRUE(fs::is_empty(base));
}

#if defined(__linux__)
TEST_F(Configuration, XdgAbsoluteOverridesAndRelativeFallbacks) {
  EnvironmentOverride home("HOME", base.string());
  EnvironmentOverride config("XDG_CONFIG_HOME", (base / "settings").string());
  EnvironmentOverride data("XDG_DATA_HOME", std::string("relative"));
  EnvironmentOverride state("XDG_STATE_HOME", (base / "persistent").string());
  EnvironmentOverride cache("XDG_CACHE_HOME", std::string("relative"));
  EnvironmentOverride runtime("XDG_RUNTIME_DIR", (base / "run").string());
  auto result = folio::config::application_paths();
  ASSERT_TRUE(std::holds_alternative<folio::config::ApplicationPaths>(result));
  const auto& paths = std::get<folio::config::ApplicationPaths>(result);
  EXPECT_EQ(paths.config, base / "settings/folio");
  EXPECT_EQ(paths.data, base / ".local/share/folio");
  EXPECT_EQ(paths.state, base / "persistent/folio");
  EXPECT_EQ(paths.logs, base / "persistent/folio/logs");
  EXPECT_EQ(paths.cache, base / ".cache/folio");
  EXPECT_EQ(paths.runtime, base / "run/folio");
  EXPECT_TRUE(fs::is_empty(base));
}
#endif
#else
TEST_F(Configuration, WindowsKnownFoldersProvideAbsoluteEffectivePaths) {
  auto result = folio::config::application_paths();
  ASSERT_TRUE(std::holds_alternative<folio::config::ApplicationPaths>(result));
  const auto& paths = std::get<folio::config::ApplicationPaths>(result);
  EXPECT_TRUE(paths.config.is_absolute());
  EXPECT_TRUE(paths.data.is_absolute());
  EXPECT_TRUE(paths.state.is_absolute());
  EXPECT_TRUE(paths.cache.is_absolute());
  EXPECT_TRUE(paths.logs.is_absolute());
  EXPECT_EQ(paths.logs, paths.state.parent_path() / "Logs");
  EXPECT_EQ(paths.config.filename(), "config");
  EXPECT_EQ(paths.data.filename(), "data");
  EXPECT_FALSE(paths.runtime); // Windows IPC uses Named Pipes.
  EXPECT_TRUE(fs::is_empty(base));
}
#endif

TEST_F(Configuration, InvalidApplicationPathsAndRelativeConfigFileAreRejected) {
  auto invalid = application;
  invalid.cache = "relative";
  auto result = folio::config::load_configuration(invalid, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(result));
  EXPECT_EQ(std::get<folio::config::ConfigurationError>(result).code,
            folio::config::ConfigurationErrorCode::InvalidPath);
  EXPECT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(
      folio::config::load_configuration(application, "folio_config.toml")));
}

TEST_F(Configuration, AccessWorkerSettingUsesTemplateDefaultsAndRejectsInvalidValues) {
  auto defaults = folio::config::configuration_defaults(application);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(defaults));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(defaults).access_workers, 4u);
  write("");
  auto missing = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(missing));
  EXPECT_EQ(std::get<folio::config::EffectiveConfig>(missing).access_workers, 4u);
  for (int workers : {1, 4, 17, 1024}) {
    write("[performance]\naccess_workers = " + std::to_string(workers));
    auto config = folio::config::load_configuration(application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(config));
    EXPECT_EQ(std::get<folio::config::EffectiveConfig>(config).access_workers, workers);
  }
  for (auto text : {"[performance]\naccess_workers = 0", "[performance]\naccess_workers = -1",
                    "[performance]\naccess_workers = 1.5", "[performance]\naccess_workers = true",
                    "[performance]\naccess_workers = '2'", "[performance]\nworkers = 2",
                    "performance = false"}) {
    write(text);
    auto config = folio::config::load_configuration(application, file);
    ASSERT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(config)) << text;
    EXPECT_EQ(std::get<folio::config::ConfigurationError>(config).code,
              folio::config::ConfigurationErrorCode::InvalidValue);
  }
}

TEST_F(Configuration, IpcSettingsDefaultOverrideAndValidate) {
  write("");
  auto defaults = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(defaults));
  const auto& value = std::get<folio::config::EffectiveConfig>(defaults);
  EXPECT_EQ(value.backend_sessions, 64u);
  EXPECT_EQ(value.backend_requests, 32u);
  EXPECT_EQ(value.backend_buffered_requests, 4u);
  EXPECT_EQ(value.frontend_push_queue_capacity, 256u);
  EXPECT_EQ(value.backend_push_queue_capacity, 256u);
  EXPECT_EQ(value.push_ack_timeout.count(), 30000);
  write("[ipc]\nbackend_sessions = 3\nbackend_requests = 5\nbackend_buffered_requests = "
        "2\nfrontend_push_queue_capacity = 7\nbackend_push_queue_capacity = "
        "9\npush_ack_timeout_ms = 1234");
  auto custom = folio::config::load_configuration(application, file);
  ASSERT_TRUE(std::holds_alternative<folio::config::EffectiveConfig>(custom));
  const auto& changed = std::get<folio::config::EffectiveConfig>(custom);
  EXPECT_EQ(changed.backend_sessions, 3u);
  EXPECT_EQ(changed.backend_requests, 5u);
  EXPECT_EQ(changed.backend_buffered_requests, 2u);
  EXPECT_EQ(changed.frontend_push_queue_capacity, 7u);
  EXPECT_EQ(changed.backend_push_queue_capacity, 9u);
  EXPECT_EQ(changed.push_ack_timeout.count(), 1234);
  for (auto key :
       {"backend_sessions", "backend_requests", "backend_buffered_requests",
        "frontend_push_queue_capacity", "backend_push_queue_capacity", "push_ack_timeout_ms"})
    for (auto invalid : {"0", "-1", "true", "1.5", "'7'"}) {
      write(std::string("[ipc]\n") + key + " = " + invalid);
      EXPECT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(
          folio::config::load_configuration(application, file)));
    }
  write("[ipc]\npush_ack_timeout_ms = 9223372036854775807");
  EXPECT_TRUE(std::holds_alternative<folio::config::ConfigurationError>(
      folio::config::load_configuration(application, file)));
}

} // namespace
} // namespace folio::application::internal
