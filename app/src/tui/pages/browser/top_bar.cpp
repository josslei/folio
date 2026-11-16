#include "tui/pages/browser/top_bar.hpp"
#include "tui/logic/navigation/navigation_manager.hpp"
#include "tui/pages/browser/entries_display.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
namespace folio::tui {
template <> const navigation::Description& navigation::description<TopBar>() {
  static const auto description = navigation::NavigableComponent::describe<TopBar>();
  return description;
}
navigation::Factory TopBar::factory(const ColorScheme& scheme) {
  return [&scheme] {
    return std::make_shared<TopBar>([](BrowserKind) {}, [] { return BrowserKind::Logs; }, scheme);
  };
}
void TopBar::set_browser_context(std::function<void(BrowserKind)> select,
                                 std::function<BrowserKind()> active,
                                 std::shared_ptr<EntriesDisplay> browsers) {
  select_ = std::move(select);
  active_ = std::move(active);
  browsers_ = std::move(browsers);
  bind_child<TopBar, LogsBrowser>(browsers_->browser_factory(BrowserKind::Logs));
  bind_child<TopBar, MemosBrowser>(browsers_->browser_factory(BrowserKind::Memos));
  bind_child<TopBar, TodosBrowser>(browsers_->browser_factory(BrowserKind::Todos));
}
TopBar::TopBar(std::function<void(BrowserKind)> select, std::function<BrowserKind()> active,
               const ColorScheme& scheme, std::shared_ptr<EntriesDisplay> browsers)
    : navigation::NavigableComponent(folio::tui::navigation::description<TopBar>()),
      browsers_(std::move(browsers)), scheme_(scheme), select_(std::move(select)),
      active_(std::move(active)) {
  if (browsers_) set_browser_context(select_, active_, browsers_);
}

std::shared_ptr<navigation::INavigable> TopBar::resolve_child(const navigation::Anchor& child,
                                                              const navigation::Context&) const {
  const auto browser = browser_kind(child);
  return browser && browsers_ ? browsers_->browser(*browser) : nullptr;
}
navigation::Target TopBar::target(BrowserKind browser, const navigation::Context& context) const {
  switch (browser) {
  case BrowserKind::Logs:
    return NavigableComponent::target<TopBar, LogsBrowser>(context);
  case BrowserKind::Memos:
    return NavigableComponent::target<TopBar, MemosBrowser>(context);
  case BrowserKind::Todos:
    return NavigableComponent::target<TopBar, TodosBrowser>(context);
  }
  throw std::logic_error("Unknown Browser kind");
}
void TopBar::host(const std::shared_ptr<navigation::INavigable>& destination,
                  const navigation::Context& context) {
  if (!browsers_ || context.depth + 1 != context.path.size())
    throw std::logic_error("Top Bar cannot host this path");
  if (!browsers_->adopt_browser(context.path.back(), destination))
    throw std::logic_error("Top Bar could not retain its declared browser");
}
void TopBar::activate(int index) {
  const auto section = static_cast<BrowserKind>(index);
  if (active_() == section) return;
  const bool content_focused = browsers_ && browsers_->Focused();
  select_(section);
  if (manager_) {
    if (browser_kind(manager_->current_location().back())) manager_->up();
    const auto& location = manager_->current_location();
    manager_->down(target(section, {location, location.size()}));
    if (content_focused) browsers_->TakeFocus();
  }
}
ftxui::Element TopBar::OnRender() {
  using namespace ftxui;
  const std::array<const char*, 3> labels{"LOGS", "MEMOS", "TODOS"};
  Elements items;
  for (int i = 0; i < 3; ++i) {
    const bool focused = i == cursor_ && Focused();
    auto item = text(std::string(focused ? "[     " : "      ") + labels[i] +
                     (focused ? "     ]" : "      "));
    if (static_cast<BrowserKind>(i) == active_())
      item = item | bgcolor(scheme_.primary_container()) | color(scheme_.on_primary_container());
    else
      item = item | color(scheme_.text_muted());
    items.push_back(item | reflect(boxes_[i]));
  }
  Elements line;
  for (int i = 0; i < 3; ++i) {
    const auto width = std::string_view(labels[i]).size() + 12;
    std::string segment;
    for (std::size_t j = 0; j < width; ++j)
      segment += "▔";
    auto part = text(segment);
    part = part | color(scheme_.primary_container());
    line.push_back(part);
  }
  line.push_back(separatorCharacter("▔") | color(scheme_.primary_container()) | flex);
  return vbox({hbox(std::move(items)), hbox(std::move(line))});
}
bool TopBar::OnEvent(ftxui::Event event) {
  using namespace ftxui;
  if (event.is_mouse()) {
    if (event.mouse().button != Mouse::Left || event.mouse().motion != Mouse::Pressed) return false;
    for (int i = 0; i < 3; ++i)
      if (boxes_[i].Contain(event.mouse().x, event.mouse().y)) {
        activate(i);
        return true;
      }
    return false;
  }
  if (!Focused()) return false;
  if (event == Event::ArrowRight)
    cursor_ = (cursor_ + 1) % 3;
  else if (event == Event::ArrowLeft)
    cursor_ = (cursor_ + 2) % 3;
  else if (event == Event::Return)
    activate(cursor_);
  else
    return false;
  return true;
}
} // namespace folio::tui
