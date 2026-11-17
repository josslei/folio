#pragma once
#include "tui/theme/color_scheme.hpp"
#include "tui/pages/browser/browser_kind.hpp"
#include "tui/logic/navigation/navigation.hpp"
#include "tui/logic/browser/browser_query_controller.hpp"
#include <ftxui/component/component_base.hpp>
namespace folio::tui {
/// Presentation of query results only; owns no Backend or request state.
class EntriesDisplay : public ftxui::ComponentBase {
public:
  EntriesDisplay(BrowserQueryController& queries, std::function<std::optional<BrowserKind>()> active,
                 const ColorScheme& scheme = default_color_scheme(),
                 std::shared_ptr<navigation::INavigable> prepared = {});
  bool adopt_browser(const navigation::Anchor& anchor,
                     const std::shared_ptr<navigation::INavigable>& browser);
  std::shared_ptr<navigation::INavigable> browser(BrowserKind kind) const;
  /// Captures only session services; remains valid after this display is destroyed.
  navigation::Factory browser_factory(BrowserKind kind) const;
  static navigation::Factory browser_factory(BrowserKind kind, BrowserQueryController& queries,
                                             const ColorScheme& scheme);
  BrowserPosition position();
  void restore_position(const BrowserPosition& position);
  ftxui::Element OnRender() override;
  ftxui::Component ActiveChild() override;
  bool OnEvent(ftxui::Event event) override;
  bool Focusable() const override;
  void set_browser_context(std::function<std::optional<BrowserKind>()> active) {
    active_ = std::move(active);
  }

private:
  std::function<std::optional<BrowserKind>()> active_;
  const ColorScheme& scheme_;
  BrowserQueryController& queries_;
  ftxui::Component& browser_slot(BrowserKind kind);
  ftxui::Component logs_;
  ftxui::Component memos_;
  ftxui::Component todos_;
};
} // namespace folio::tui
