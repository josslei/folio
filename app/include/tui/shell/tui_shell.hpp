#pragma once
#include "tui/theme/color_scheme.hpp"
#include "tui/logic/navigation/navigation_manager.hpp"
#include "tui/logic/navigation/navigable_component.hpp"
#include "tui/shell/bottom_bar.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <string>
namespace folio::tui {
class BrowserPage;
/// Application-wide composition and Quit handling. Hosts a page and the Bottom
/// Bar. Feedback is read after page rendering; callback must outlive the Shell.
class TuiShell : public navigation::NavigableComponent {
public:
  using NavigationChildren = navigation::Children<BrowserPage>;
  using BrowserFactory =
      std::function<std::shared_ptr<BrowserPage>(const std::shared_ptr<navigation::INavigable>&)>;
  TuiShell(ftxui::Component page, std::function<ftxui::Element()> feedback,
           std::function<void()> quit, const ColorScheme& scheme = default_color_scheme(),
           BrowserFactory browser_factory = {}, navigation::Location initial_location = {});
  ~TuiShell() override;
  std::shared_ptr<navigation::INavigable>
  resolve_child(const navigation::Anchor& child, const navigation::Context& context) const override;
  ftxui::Component page() const { return page_; }
  navigation::Manager* navigation_manager() { return navigation_ ? &*navigation_ : nullptr; }
  void host(const std::shared_ptr<navigation::INavigable>& destination,
            const navigation::Context& context) override;
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;

private:
  BrowserFactory browser_factory_;
  std::optional<navigation::Manager> navigation_;
  const ColorScheme& scheme_;
  ftxui::Component page_;
  std::function<ftxui::Element()> feedback_;
  std::function<void()> quit_;
  std::shared_ptr<BottomBar> bottom_bar_;
};
template <> const navigation::Description& navigation::description<TuiShell>();
} // namespace folio::tui
