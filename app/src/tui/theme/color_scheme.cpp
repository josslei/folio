#include "tui/theme/color_scheme.hpp"
#include <cstdint>
namespace folio::tui {
namespace {
using Palette = std::array<std::uint32_t, 22>;
constexpr Palette nocturne{0x58AEA4, 0x071715, 0x295651, 0xE0F2EB, 0x9FADC3, 0x121A24,
                           0xC49B66, 0x24180C, 0x111516, 0x293334, 0x354244, 0xE7E7DC,
                           0xA7B7B4, 0x63716F, 0x394745, 0x58AEA4, 0x071715, 0x8C9996,
                           0x91BE98, 0xD5AF70, 0xDE8E89, 0x8EAED4};
constexpr Palette afterglow{0xC7A46C, 0x21180C, 0x55452E, 0xF0DFC0, 0x8CA79A, 0x101D17,
                            0xC58E82, 0x2A1110, 0x1A1917, 0x33302A, 0x454037, 0xEBE2D1,
                            0xBDB09B, 0x756B59, 0x484238, 0xD3B57F, 0x21180C, 0xA69B8A,
                            0x9EBC96, 0xD6B57B, 0xDE948A, 0x94AFC1};
ColorScheme make_scheme(const Palette& palette) {
  ColorScheme::Colors colors;
  for (std::size_t i = 0; i < palette.size(); ++i) {
    const auto rgb = palette[i];
    colors[i] = ftxui::Color::RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
  }
  return ColorScheme(std::move(colors));
}
const std::array themes{
    ThemeDefinition{"hweiluh-nocturne", "Hweiluh Nocturne", make_scheme(nocturne)},
    ThemeDefinition{"gueidaang-afterglow", "Gueidaang Afterglow", make_scheme(afterglow)}};
} // namespace
const ThemeDefinition* find_theme(std::string_view id) {
  for (const auto& theme : themes)
    if (theme.id == id) return &theme;
  return nullptr;
}
const ColorScheme& default_color_scheme() {
  return themes.front().scheme;
}
} // namespace folio::tui
