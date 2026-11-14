#include "tui/pages/browser/browser_page.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
namespace folio::tui {
BrowserPage::BrowserPage(BrowserQueryController& queries, std::function<void()> wake,
                         std::chrono::milliseconds loading_delay,
                         std::chrono::milliseconds loaded_duration)
    : queries_(queries), loading_status_(std::make_shared<LoadingStatus>(std::move(wake), loading_delay, loaded_duration)),
      top_bar_(std::make_shared<TopBar>([this](core::RecordType type) { select(type); })),
      entries_(std::make_shared<EntriesDisplay>(queries)) {
  Add(top_bar_);
  Add(entries_);
  select(core::RecordType::Log);
}
ftxui::Element BrowserPage::feedback() const {
  if (!feedback_.empty()) return ftxui::text(feedback_);
  return loading_status_->Render();
}
void BrowserPage::select(core::RecordType type) {
  ++feedback_generation_;
  awaiting_result_ = true;
  loading_status_->begin(feedback_generation_);
  queries_.select(type);
}
ftxui::Element BrowserPage::OnRender() {
  using namespace ftxui;
  if (awaiting_result_ && !queries_.loading()) {
    loading_status_->received(feedback_generation_);
    loading_status_->complete(feedback_generation_);
    awaiting_result_ = false;
  }
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

  return vbox({top_bar_->Render(), entries_->Render()}) |
         flex;
}
} // namespace folio::tui
