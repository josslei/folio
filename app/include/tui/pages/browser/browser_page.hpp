#pragma once
#include "tui/logic/browser/browser_query_controller.hpp"
#include "tui/pages/browser/top_bar.hpp"
#include "tui/pages/browser/entries_display.hpp"
#include "tui/components/loading_status.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
namespace folio::tui {
/// Browser-specific header, content and feedback. Borrows query state;
/// caller keeps it alive beyond the page and its child components.
class BrowserPage : public ftxui::ComponentBase {
public:
  BrowserPage(BrowserQueryController& queries, std::function<void()> wake,
              std::chrono::milliseconds loading_delay = std::chrono::milliseconds(100),
              std::chrono::milliseconds loaded_duration = std::chrono::seconds(1));
  ftxui::Element OnRender() override;
  /// Shell reads feedback after rendering the page; no Shell ownership here.
  ftxui::Element feedback() const;

private:
  void select(core::RecordType type);
  BrowserQueryController& queries_;
  std::shared_ptr<LoadingStatus> loading_status_;
  std::size_t feedback_generation_ = 0;
  bool awaiting_result_ = false;
  std::string feedback_;
  std::shared_ptr<TopBar> top_bar_;
  std::shared_ptr<EntriesDisplay> entries_;
};
} // namespace folio::tui
