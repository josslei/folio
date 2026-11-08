#pragma once
#include <filesystem>
namespace folio::io {
bool has_nul(const std::filesystem::path& path);
/// Lexical component comparison; Windows uses ordinal case-insensitive matching.
/// Performs no filesystem lookup, symlink resolution or canonicalization.
bool component_equal(const std::filesystem::path& left, const std::filesystem::path& right);
/// Tests component-prefix containment, including equality. Inputs must already
/// be normalized by callers; this alone is not a filesystem confinement check.
bool contains_path(const std::filesystem::path& parent, const std::filesystem::path& child);
/// True when either lexical path contains the other, using component_equal rules.
bool overlaps(const std::filesystem::path& left, const std::filesystem::path& right);
} // namespace folio::io
