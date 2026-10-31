#pragma once

#include <folio/config/configuration.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>

namespace folio::logging {

enum class ProcessRole { Backend, Frontend };

// Call once after selecting the process role, before loading configuration.
// Uses platform paths only; never reads TOML. Failure leaves a silent default
// logger and returns nullopt, without making application startup fail.
[[nodiscard]] std::optional<std::filesystem::path> initialize(ProcessRole role) noexcept;
// Explicit root injection for isolated tests and embedded entry points.
[[nodiscard]] std::optional<std::filesystem::path> initialize(
    ProcessRole role, const std::filesystem::path& log_root) noexcept;

// Updates the existing default logger, without replacing its sink or file.
void set_level(config::LogLevel level) noexcept;
// Explicit flush/release after lifecycle cleanup and all logging threads stop.
// A silent default remains available for late diagnostic calls. No Catalog work.
void shutdown() noexcept;

}  // namespace folio::logging
