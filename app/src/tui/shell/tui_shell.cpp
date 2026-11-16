#include "tui/shell/tui_shell.hpp"
#include "tui/pages/browser/browser_page.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>
namespace folio::tui {
template <> const navigation::Description& navigation::description<TuiShell>() {
  static const auto description = navigation::NavigableComponent::describe<TuiShell>();
  return description;
}
namespace {
constexpr int minimum_width = 80;
constexpr int minimum_height = 24;
bool too_small() {
  const auto size = ftxui::Terminal::Size();
  return size.dimx < minimum_width || size.dimy < minimum_height;
}
} // namespace
TuiShell::TuiShell(ftxui::Component page, std::function<ftxui::Element()> feedback,
                   std::function<void()> quit, const ColorScheme& scheme,
                   BrowserFactory browser_factory, navigation::Location initial_location)
    : navigation::NavigableComponent(folio::tui::navigation::description<TuiShell>()),
      browser_factory_(std::move(browser_factory)), scheme_(scheme), page_(std::move(page)),
      feedback_(std::move(feedback)), quit_(std::move(quit)),
      bottom_bar_(std::make_shared<BottomBar>(feedback_, scheme)) {
  if (page_) Add(page_);
  Add(bottom_bar_);
  if (!browser_factory_)
    if (auto* browser = dynamic_cast<BrowserPage*>(page_.get()))
      browser_factory_ = browser->factory();
  if (browser_factory_) {
    bind_child<TuiShell, BrowserPage>([factory = browser_factory_] { return factory({}); });
    if (initial_location.empty()) {
      auto browser = std::dynamic_pointer_cast<BrowserPage>(page_);
      if (!browser) throw std::logic_error("Missing initial Browser location");
      const auto& top_registration = browser->description().children.front()();
      const navigation::Location prefix{
          navigation::description<BrowserPage>().anchor,
          top_registration.anchor.with_factory(top_registration.create)};
      auto top = browser->resolve_child(prefix.back(), {prefix, 1});
      initial_location = top->children({prefix, 2}).front().destination();
    }
    navigation_.emplace(*this, std::move(initial_location),
                        [this](const navigation::Location& location, const std::any* position) {
                          if (auto* active = dynamic_cast<BrowserPage*>(page_.get())) {
                            if (navigation_) active->set_navigation_manager(&*navigation_);
                            active->navigation_changed(location, position);
                          }
                        });
    auto* browser = dynamic_cast<BrowserPage*>(page_.get());
    browser->set_navigation_manager(&*navigation_);
    // Startup keeps Top Bar focus. History Start remains Logs Root separately;
    // Up updates the current History slot to the Top Bar ancestor.
    navigation_->up();
  }
}
TuiShell::~TuiShell() {
  if (auto* browser = dynamic_cast<BrowserPage*>(page_.get()))
    browser->set_navigation_manager(nullptr);
}

std::shared_ptr<navigation::INavigable> TuiShell::resolve_child(const navigation::Anchor& child,
                                                                const navigation::Context&) const {
  auto browser = std::dynamic_pointer_cast<BrowserPage>(page_);
  if (!child.is<BrowserPage>()) return {};
  return browser;
}
void TuiShell::host(const std::shared_ptr<navigation::INavigable>& destination,
                    const navigation::Context& context) {
  if (page_ || context.path.empty() || !context.path.front().is<BrowserPage>())
    throw std::logic_error("Shell cannot host this path");
  auto candidate = std::dynamic_pointer_cast<BrowserPage>(destination);
  if (!candidate) candidate = std::dynamic_pointer_cast<BrowserPage>(browser_factory_(destination));
  if (!candidate) throw std::logic_error("Browser factory returned an empty page");
  Add(candidate);
  page_ = std::move(candidate);
}
ftxui::Element TuiShell::OnRender() {
  using namespace ftxui;
  auto page_content =
      too_small() ? vbox({text("Terminal too small") | hcenter, text("Minimum: 80 x 24") | hcenter,
                          text("Resize to continue") | hcenter}) |
                        center
                  : page_->Render();
  auto content = hbox({text(" "), page_content | flex, text(" ")});
  return vbox({text(""), content | flex, bottom_bar_->Render(), text("")}) |
         bgcolor(scheme_.surface()) | color(scheme_.on_surface());
}
bool TuiShell::OnEvent(ftxui::Event event) {
  if (event == ftxui::Event::CtrlQ) {
    quit_();
    return true;
  }
  // Keep the hosted page and its cursor intact while it is not visible.
  if (too_small()) return false;
  if (event == ftxui::Event::Escape) {
    if (navigation_) navigation_->contextual_back();
    return true;
  }
  return ComponentBase::OnEvent(event);
}
} // namespace folio::tui
