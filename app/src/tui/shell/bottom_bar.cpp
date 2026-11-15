#include "tui/shell/bottom_bar.hpp"
namespace folio::tui {
ftxui::Element BottomBar::OnRender() {
  using namespace ftxui;
  return hbox({text(" "), feedback_() | flex, text(" C-q Quit ")}) |
         bgcolor(scheme_.surface_container()) | color(scheme_.on_surface());
}
} // namespace folio::tui
