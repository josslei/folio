#include <folio/io/path.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace folio::io {
namespace fs = std::filesystem;
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
}
