#include "tui/shell/tui_shell.hpp"
#include <ftxui/component/event.hpp>
namespace folio::tui {
TuiShell::TuiShell(ftxui::Component page, std::function<ftxui::Element()> feedback,
                   std::function<void()> quit)
    : page_(std::move(page)), feedback_(std::move(feedback)), quit_(std::move(quit)),
      bottom_bar_(std::make_shared<BottomBar>(feedback_)) {
  Add(page_);
  Add(bottom_bar_);
}
ftxui::Element TuiShell::OnRender() {
  using namespace ftxui;
  auto content = hbox({text(" "), page_->Render() | flex, text(" ")});
  return vbox({text(""), content | flex, bottom_bar_->Render(), text("")});
}
bool TuiShell::OnEvent(ftxui::Event event) {
  if (event == ftxui::Event::CtrlQ) {
    quit_();
    return true;
  }
  return ComponentBase::OnEvent(event);
}
} // namespace folio::tui
