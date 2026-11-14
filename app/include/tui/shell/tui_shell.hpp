#pragma once
#include "tui/shell/bottom_bar.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <string>
namespace folio::tui {
/// Application-wide composition and Quit handling. Hosts a page and the Bottom
/// Bar. Feedback is read after page rendering; callback must outlive the Shell.
class TuiShell : public ftxui::ComponentBase {
public:
  TuiShell(ftxui::Component page, std::function<ftxui::Element()> feedback,
           std::function<void()> quit);
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;

private:
  ftxui::Component page_;
  std::function<ftxui::Element()> feedback_;
  std::function<void()> quit_;
  std::shared_ptr<BottomBar> bottom_bar_;
};
} // namespace folio::tui
