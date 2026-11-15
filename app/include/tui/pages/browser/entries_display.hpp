#pragma once
#include "tui/theme/color_scheme.hpp"
#include "tui/logic/browser/browser_query_controller.hpp"
#include <ftxui/component/component_base.hpp>
namespace folio::tui {
/// Presentation of query results only; owns no Backend or request state.
class EntriesDisplay : public ftxui::ComponentBase {
public:
  explicit EntriesDisplay(const BrowserQueryController& queries, const ColorScheme& scheme = default_color_scheme());
  ftxui::Element OnRender() override;
  ftxui::Component ActiveChild() override;
  bool OnEvent(ftxui::Event event) override;
  bool Focusable() const override;
private:
  const ColorScheme& scheme_;
  const BrowserQueryController& queries_;
  ftxui::Component logs_;
  ftxui::Component memos_;
  ftxui::Component todos_;
};
} // namespace folio::tui
