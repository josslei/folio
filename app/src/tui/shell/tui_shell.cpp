#include "tui/shell/tui_shell.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>
namespace folio::tui {
namespace {
constexpr int minimum_width = 80;
constexpr int minimum_height = 24;
bool too_small() {
  const auto size = ftxui::Terminal::Size();
  return size.dimx < minimum_width || size.dimy < minimum_height;
}
} // namespace
TuiShell::TuiShell(ftxui::Component page, std::function<ftxui::Element()> feedback,
                   std::function<void()> quit, const ColorScheme& scheme)
    : scheme_(scheme), page_(std::move(page)), feedback_(std::move(feedback)), quit_(std::move(quit)),
      bottom_bar_(std::make_shared<BottomBar>(feedback_, scheme)) {
  Add(page_);
  Add(bottom_bar_);
}
ftxui::Element TuiShell::OnRender() {
  using namespace ftxui;
  auto page_content = too_small()
      ? vbox({text("Terminal too small") | hcenter,
              text("Minimum: 80 x 24") | hcenter,
              text("Resize to continue") | hcenter}) | center
      : page_->Render();
  auto content = hbox({text(" "), page_content | flex, text(" ")});
  return vbox({text(""), content | flex, bottom_bar_->Render(), text("")}) | bgcolor(scheme_.surface()) | color(scheme_.on_surface());
}
bool TuiShell::OnEvent(ftxui::Event event) {
  if (event == ftxui::Event::CtrlQ) {
    quit_();
    return true;
  }
  // Keep the hosted page and its cursor intact while it is not visible.
  if (too_small()) return false;
  return ComponentBase::OnEvent(event);
}
} // namespace folio::tui
