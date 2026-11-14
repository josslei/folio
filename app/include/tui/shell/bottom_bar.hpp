#pragma once
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <string>
namespace folio::tui {
/// Fixed bottom region: feedback and the persistent Quit reminder.
class BottomBar : public ftxui::ComponentBase {
public:
  explicit BottomBar(std::function<ftxui::Element()> feedback) : feedback_(std::move(feedback)) {}
  ftxui::Element OnRender() override;
private:
  std::function<ftxui::Element()> feedback_;
};
} // namespace folio::tui
