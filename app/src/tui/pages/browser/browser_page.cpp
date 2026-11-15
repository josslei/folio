#include "tui/pages/browser/browser_page.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
namespace folio::tui {
BrowserPage::BrowserPage(BrowserQueryController& queries, std::function<void()> wake,
                         std::chrono::milliseconds loading_delay,
                         std::chrono::milliseconds loaded_duration, const ColorScheme& scheme)
    : scheme_(scheme), queries_(queries), loading_status_(std::make_shared<LoadingStatus>(std::move(wake), loading_delay, loaded_duration)),
      top_bar_(std::make_shared<TopBar>([this](core::RecordType type) { select(type); }, scheme)),
      entries_(std::make_shared<EntriesDisplay>(queries, scheme)) {
  navigation_ = ftxui::Container::Vertical({top_bar_, entries_});
  Add(navigation_);
  select(core::RecordType::Log);
}
ftxui::Element BrowserPage::feedback() {
  if (awaiting_result_ && !queries_.loading()) {
    loading_status_->received(feedback_generation_);
    loading_status_->complete(feedback_generation_);
    awaiting_result_ = false;
  }
  if (!feedback_.empty()) {
    const bool failed = queries_.result() && std::holds_alternative<application::ApplicationError>(*queries_.result());
    return ftxui::text(feedback_) | ftxui::color(failed ? scheme_.error() : scheme_.warning());
  }
  return loading_status_->Render();
}
void BrowserPage::select(core::RecordType type) {
  ++feedback_generation_;
  awaiting_result_ = true;
  loading_status_->begin(feedback_generation_);
  queries_.select(type);
}
bool BrowserPage::OnEvent(ftxui::Event event) {
  // Mouse activation must not invoke container focus movement (including wheel events).
  if (event.is_mouse()) return top_bar_->OnEvent(event);
  if (!entries_->Focusable()) navigation_->SetActiveChild(top_bar_.get());
  return navigation_->OnEvent(event);
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
