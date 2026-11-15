#pragma once
#include <ftxui/screen/color.hpp>
#include <array>
#include <string_view>
#include <utility>

namespace folio::tui {
/// Immutable semantic colors. FTXUI Color also supports ANSI and terminal defaults.
/// Components borrow schemes: keep a custom scheme alive beyond its component tree.
/// Nonvisual presentation data; deliberately not an FTXUI Component.
class ColorScheme {
private:
  enum class ColorRole {
    primary,
    on_primary,
    primary_container,
    on_primary_container,
    secondary,
    on_secondary,
    tertiary,
    on_tertiary,
    surface,
    surface_container,
    surface_container_high,
    on_surface,
    on_surface_variant,
    outline,
    outline_variant,
    focus,
    on_focus,
    text_muted,
    success,
    warning,
    error,
    info,
    count
  };

public:
  using Colors = std::array<ftxui::Color, static_cast<std::size_t>(ColorRole::count)>;
  explicit ColorScheme(Colors colors) : colors_(std::move(colors)) {}
  ftxui::Color primary() const { return colors_[static_cast<std::size_t>(ColorRole::primary)]; }
  ftxui::Color on_primary() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_primary)];
  }
  ftxui::Color primary_container() const {
    return colors_[static_cast<std::size_t>(ColorRole::primary_container)];
  }
  ftxui::Color on_primary_container() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_primary_container)];
  }
  ftxui::Color secondary() const { return colors_[static_cast<std::size_t>(ColorRole::secondary)]; }
  ftxui::Color on_secondary() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_secondary)];
  }
  ftxui::Color tertiary() const { return colors_[static_cast<std::size_t>(ColorRole::tertiary)]; }
  ftxui::Color on_tertiary() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_tertiary)];
  }
  ftxui::Color surface() const { return colors_[static_cast<std::size_t>(ColorRole::surface)]; }
  ftxui::Color surface_container() const {
    return colors_[static_cast<std::size_t>(ColorRole::surface_container)];
  }
  ftxui::Color surface_container_high() const {
    return colors_[static_cast<std::size_t>(ColorRole::surface_container_high)];
  }
  ftxui::Color on_surface() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_surface)];
  }
  ftxui::Color on_surface_variant() const {
    return colors_[static_cast<std::size_t>(ColorRole::on_surface_variant)];
  }
  ftxui::Color outline() const { return colors_[static_cast<std::size_t>(ColorRole::outline)]; }
  ftxui::Color outline_variant() const {
    return colors_[static_cast<std::size_t>(ColorRole::outline_variant)];
  }
  ftxui::Color focus() const { return colors_[static_cast<std::size_t>(ColorRole::focus)]; }
  ftxui::Color on_focus() const { return colors_[static_cast<std::size_t>(ColorRole::on_focus)]; }
  ftxui::Color text_muted() const {
    return colors_[static_cast<std::size_t>(ColorRole::text_muted)];
  }
  ftxui::Color success() const { return colors_[static_cast<std::size_t>(ColorRole::success)]; }
  ftxui::Color warning() const { return colors_[static_cast<std::size_t>(ColorRole::warning)]; }
  ftxui::Color error() const { return colors_[static_cast<std::size_t>(ColorRole::error)]; }
  ftxui::Color info() const { return colors_[static_cast<std::size_t>(ColorRole::info)]; }

private:
  Colors colors_;
};

/// Theme identity and presentation data; family/variant are derived from id.
struct ThemeDefinition {
  std::string_view id;
  std::string_view display_name;
  ColorScheme scheme;
};
/// Process-lifetime immutable definitions. Unknown IDs return nullptr, never fallback.
const ThemeDefinition* find_theme(std::string_view id);
const ColorScheme& default_color_scheme();
} // namespace folio::tui
