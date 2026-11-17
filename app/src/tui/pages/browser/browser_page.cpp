#include "tui/pages/browser/browser_page.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <stdexcept>
namespace folio::tui {
navigation::Location BrowserPage::initial_location(BrowserQueryController& queries,
                                                   const ColorScheme& scheme) {
  // Fixed application policy, independent of session services or live UI objects.
  // TODO(Navigation): Revisit Browser/Logs/Root History Start when Main Page is designed.
  static const navigation::Location history_start{
      navigation::description<BrowserPage>().anchor,
      navigation::description<TopBar>().anchor, browser_anchor(BrowserKind::Logs)};
  auto bound = history_start;
  bound[1] = bound[1].with_factory(TopBar::factory(scheme));
  bound[2] = bound[2].with_factory(
      EntriesDisplay::browser_factory(BrowserKind::Logs, queries, scheme));
  return bound;
}
template <> const navigation::Description& navigation::description<BrowserPage>() {
  static const auto description = navigation::NavigableComponent::describe<BrowserPage>();
  return description;
}
BrowserPage::Factory BrowserPage::factory(BrowserQueryController& queries,
                                          std::function<void()> wake, const ColorScheme& scheme,
                                          std::chrono::milliseconds loading_delay,
                                          std::chrono::milliseconds loaded_duration) {
  return [&queries, wake = std::move(wake), &scheme, loading_delay,
          loaded_duration](const std::shared_ptr<navigation::INavigable>& prepared) {
    auto page = std::make_shared<BrowserPage>(ConstructionKey{}, queries, wake, loading_delay,
                                            loaded_duration, scheme, prepared);
    page->bind_browser_context(page);
    return page;
  };
}
BrowserPage::BrowserPage(ConstructionKey, BrowserQueryController& queries, std::function<void()> wake,
                         std::chrono::milliseconds loading_delay,
                         std::chrono::milliseconds loaded_duration, const ColorScheme& scheme,
                         std::shared_ptr<navigation::INavigable> prepared)
    : navigation::NavigableComponent(folio::tui::navigation::description<BrowserPage>()),
      factory_(factory(queries, wake, scheme, loading_delay, loaded_duration)), scheme_(scheme),
      queries_(queries), loading_status_(std::make_shared<LoadingStatus>(
                             std::move(wake), loading_delay, loaded_duration)),
      entries_(std::make_shared<EntriesDisplay>(
          queries, [] { return std::optional<BrowserKind>{}; }, scheme, prepared)),
      top_bar_(std::dynamic_pointer_cast<TopBar>(prepared)) {
  if (!top_bar_)
    top_bar_ = std::make_shared<TopBar>([](BrowserKind) {},
                                        [] { return std::optional<BrowserKind>{}; }, scheme, entries_);
  else
    top_bar_->set_browser_context([](BrowserKind) {},
                                  [] { return std::optional<BrowserKind>{}; }, entries_);
  navigation_ = ftxui::Container::Vertical({top_bar_, entries_});
  Add(navigation_);
  bind_child<BrowserPage, TopBar>(TopBar::factory(scheme));
}
BrowserPage::~BrowserPage() {
  top_bar_->set_navigation_manager(nullptr);
}
void BrowserPage::bind_browser_context(std::weak_ptr<BrowserPage> weak) {
  auto active = [weak]() -> std::optional<BrowserKind> {
    if (auto owner = weak.lock()) return owner->active_browser();
    return std::nullopt;
  };
  entries_->set_browser_context(active);
  top_bar_->set_browser_context([weak](BrowserKind) {
    if (auto owner = weak.lock()) owner->remember_position();
  }, active, entries_);
}
ftxui::Element BrowserPage::feedback() {
  if (awaiting_result_ && !queries_.loading()) {
    loading_status_->received(feedback_generation_);
    loading_status_->complete(feedback_generation_);
    awaiting_result_ = false;
  }
  if (!feedback_.empty()) {
    const bool failed = queries_.result() &&
                        std::holds_alternative<application::ApplicationError>(*queries_.result());
    return ftxui::text(feedback_) | ftxui::color(failed ? scheme_.error() : scheme_.warning());
  }
  return loading_status_->Render();
}
namespace {
core::RecordType query_type(BrowserKind section) {
  switch (section) {
  case BrowserKind::Logs:
    return core::RecordType::Log;
  case BrowserKind::Memos:
    return core::RecordType::Memo;
  case BrowserKind::Todos:
    return core::RecordType::Todo;
  }
  throw std::logic_error("Unknown Browser Section");
}
} // namespace

std::shared_ptr<navigation::INavigable>
BrowserPage::resolve_child(const navigation::Anchor& child, const navigation::Context&) const {
  if (!child.is<TopBar>()) return {};
  return top_bar_;
}
void BrowserPage::set_navigation_manager(navigation::Manager* manager) {
  if (manager_ == manager) return;
  manager_ = manager;
  top_bar_->set_navigation_manager(manager);
  if (manager_) remember_position();
}
BrowserKind BrowserPage::active_browser() const {
  if (manager_ && !manager_->current_location().empty()) {
    const auto& location = manager_->current_location();
    if (const auto browser = browser_kind(location.back())) return *browser;
    // Up moves the navigation cursor to Top Bar without clearing its content.
    // The Manager owns this parent context, not a second Page selection field.
    const auto saved = manager_->restored_position();
    if (const auto* browser = std::any_cast<BrowserKind>(&saved)) return *browser;
  }
  return BrowserKind::Logs; // Before Shell binds the initial navigation::Location only.
}
void BrowserPage::navigation_changed(const navigation::Location& location,
                                     const std::any& position) {
  const auto target_browser = browser_kind(location.back());
  const auto browser = target_browser ? *target_browser : active_browser();
  // Focus-only Up/Down must not start another Backend query or reset feedback.
  if (queries_.query_type() != query_type(browser) || (!queries_.loading() && !queries_.result()))
    select(browser);
  if (target_browser) {
    const auto* cursor = std::any_cast<BrowserPosition>(&position);
    entries_->restore_position(cursor ? *cursor : BrowserPosition{});
    if (manager_) remember_position();
  } else {
    if (navigation_->ActiveChild() == entries_) top_bar_->focus_browser(browser);
    navigation_->SetActiveChild(top_bar_.get());
  }
}
void BrowserPage::remember_position() {
  if (!manager_) return;
  const auto& location = manager_->current_location();
  const auto browser = browser_kind(location.back());
  if (!browser) return;
  manager_->remember_position(entries_->position());
  auto parent = location;
  parent.pop_back();
  manager_->remember_position(parent, *browser);
}
void BrowserPage::select(BrowserKind section) {
  ++feedback_generation_;
  awaiting_result_ = true;
  loading_status_->begin(feedback_generation_);
  queries_.select(query_type(section));
}
bool BrowserPage::OnEvent(ftxui::Event event) {
  // Mouse activation does not move the Logical Cursor.
  if (event.is_mouse()) return top_bar_->OnEvent(event);
  if (!entries_->Focusable()) navigation_->SetActiveChild(top_bar_.get());
  const bool was_content = navigation_->ActiveChild() == entries_;
  const bool handled = navigation_->OnEvent(event);
  const bool is_content = navigation_->ActiveChild() == entries_;
  if (manager_ && was_content != is_content) {
    if (is_content) {
      if (manager_->current_location().back().is<TopBar>())
        manager_->down(top_bar_->target(
            active_browser(), {manager_->current_location(), manager_->current_location().size()}));
    } else {
      top_bar_->focus_browser(active_browser());
      remember_position();
      manager_->up();
    }
  }
  remember_position();
  return handled;
}
ftxui::Element BrowserPage::OnRender() {
  using namespace ftxui;
  feedback_.clear();
  auto& message = feedback_;
  if (queries_.result()) {
    if (const auto* error = std::get_if<application::ApplicationError>(&*queries_.result()))
      message = error->message;
    else {
      const auto& value =
          std::get<application::Annotated<std::vector<core::RecordMetadata>>>(*queries_.result());
      if (!value.annotation.empty()) message = value.annotation.front().message;
    }
  }

  if (!entries_->Focusable()) navigation_->SetActiveChild(top_bar_.get());
  return navigation_->Render() | flex;
}
} // namespace folio::tui
