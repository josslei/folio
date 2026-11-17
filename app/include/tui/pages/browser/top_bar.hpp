#pragma once
#include "tui/theme/color_scheme.hpp"
#include "tui/pages/browser/browser_kind.hpp"
#include "tui/logic/navigation/navigable_component.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/box.hpp>
#include <array>
#include <functional>
namespace folio::tui {
namespace navigation {
class Manager;
}
class EntriesDisplay;
class LogsBrowser;
class MemosBrowser;
class TodosBrowser;
/// Structural parent of the three browsers; tab activation and keyboard cursor.
class TopBar : public navigation::NavigableComponent {
public:
  using NavigationChildren = navigation::Children<LogsBrowser, MemosBrowser, TodosBrowser>;
  /// Bind the session-lived theme; no parent UI instance is captured.
  static navigation::Factory factory(const ColorScheme& scheme);

  TopBar(std::function<void(BrowserKind)> select, std::function<std::optional<BrowserKind>()> active,
         const ColorScheme& scheme = default_color_scheme(),
         std::shared_ptr<EntriesDisplay> browsers = {});
  std::shared_ptr<navigation::INavigable>
  resolve_child(const navigation::Anchor& child, const navigation::Context& context) const override;
  void host(const std::shared_ptr<navigation::INavigable>& destination,
            const navigation::Context& context) override;
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  /// Bind Page-owned composition after independent factory construction.
  void set_browser_context(std::function<void(BrowserKind)> select,
                           std::function<std::optional<BrowserKind>()> active,
                           std::shared_ptr<EntriesDisplay> browsers);
  void set_navigation_manager(navigation::Manager* manager) { manager_ = manager; }
  bool Focusable() const override { return true; }
  /// Returning from browser content targets its corresponding tab, without activation.
  void focus_browser(BrowserKind browser) { cursor_ = static_cast<int>(browser); }
  navigation::Target target(BrowserKind browser, const navigation::Context& context) const;

private:
  navigation::Manager* manager_ = nullptr;
  std::shared_ptr<EntriesDisplay> browsers_;
  const ColorScheme& scheme_;
  void activate(int index);
  std::function<void(BrowserKind)> select_;
  std::function<std::optional<BrowserKind>()> active_;
  int cursor_ = 0;
  std::array<ftxui::Box, 3> boxes_{};
};
template <> const navigation::Description& navigation::description<TopBar>();
} // namespace folio::tui
