#pragma once
#include <filesystem>
namespace folio::io {
bool has_nul(const std::filesystem::path& path);
bool component_equal(const std::filesystem::path& left, const std::filesystem::path& right);
bool contains_path(const std::filesystem::path& parent, const std::filesystem::path& child);
bool overlaps(const std::filesystem::path& left, const std::filesystem::path& right);
}
