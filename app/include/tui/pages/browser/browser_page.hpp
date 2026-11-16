#pragma once
#include "tui/theme/color_scheme.hpp"
#include "tui/logic/navigation/navigation_manager.hpp"
#include "tui/logic/navigation/navigable_component.hpp"
#include "tui/logic/browser/browser_query_controller.hpp"
#include "tui/pages/browser/top_bar.hpp"
#include "tui/pages/browser/entries_display.hpp"
#include "tui/components/loading_status.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
namespace folio::tui {
/// Browser-specific header, content and feedback. Borrows query state;
/// caller keeps it alive beyond the page and its child components.
class BrowserPage : public navigation::NavigableComponent {
public:
  using NavigationChildren = navigation::Children<TopBar>;
  /// UI-owned composition factory; captures only session-lived services and values.
  using Factory =
      std::function<std::shared_ptr<BrowserPage>(const std::shared_ptr<navigation::INavigable>&)>;
  static Factory factory(BrowserQueryController& queries, std::function<void()> wake,
                         const ColorScheme& scheme = default_color_scheme(),
                         std::chrono::milliseconds loading_delay = std::chrono::milliseconds(100),
                         std::chrono::milliseconds loaded_duration = std::chrono::seconds(1));
  Factory factory() const { return factory_; }
  static navigation::Location initial_location(BrowserQueryController& queries,
                                               const ColorScheme& scheme);

  static constexpr bool navigation_visitable = false;
  BrowserPage(BrowserQueryController& queries, std::function<void()> wake,
              std::chrono::milliseconds loading_delay = std::chrono::milliseconds(100),
              std::chrono::milliseconds loaded_duration = std::chrono::seconds(1),
              const ColorScheme& scheme = default_color_scheme(),
              std::shared_ptr<navigation::INavigable> prepared = {});
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  /// Shell reads feedback after rendering the page; no Shell ownership here.
  ftxui::Element feedback();
  std::shared_ptr<navigation::INavigable>
  resolve_child(const navigation::Anchor& child, const navigation::Context& context) const override;
  /// Shell binds its manager; unbind before Shell destruction.
  void set_navigation_manager(navigation::Manager* manager);
  void navigation_changed(const navigation::Location& location, const std::any* position);
  BrowserKind active_browser() const;

private:
  Factory factory_;
  const ColorScheme& scheme_;
  void select(BrowserKind section);
  void remember_position();
  navigation::Manager* manager_ = nullptr;
  BrowserQueryController& queries_;
  std::shared_ptr<LoadingStatus> loading_status_;
  std::size_t feedback_generation_ = 0;
  bool awaiting_result_ = false;
  std::string feedback_;
  ftxui::Component navigation_;
  std::shared_ptr<EntriesDisplay> entries_;
  std::shared_ptr<TopBar> top_bar_;
};
template <> const navigation::Description& navigation::description<BrowserPage>();
} // namespace folio::tui
