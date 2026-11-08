#pragma once

// Private test access to a production helper implemented in logging.cpp.
// This separate declaration lets filename/collision tests inject a timestamp and
// PID deterministically; it is not a public API or a separate Logging module.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace folio::logging::internal {
// Reserves an empty file exclusively. Timestamp/PID injection makes naming and
// collision tests deterministic; callers provide an already-created directory.
std::optional<std::filesystem::path>
reserve_session_file(const std::filesystem::path& directory,
                     std::chrono::system_clock::time_point timestamp, std::uint64_t pid);
} // namespace folio::logging::internal
