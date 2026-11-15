#pragma once
#include "tui/theme/color_scheme.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <string>
namespace folio::tui {
/// Fixed bottom region: feedback and the persistent Quit reminder.
class BottomBar : public ftxui::ComponentBase {
public:
  explicit BottomBar(std::function<ftxui::Element()> feedback, const ColorScheme& scheme = default_color_scheme()) : scheme_(scheme), feedback_(std::move(feedback)) {}
  ftxui::Element OnRender() override;
private:
  const ColorScheme& scheme_;
  std::function<ftxui::Element()> feedback_;
};
} // namespace folio::tui
