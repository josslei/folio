#include "tui/logic/browser/browser_query_controller.hpp"

namespace folio::tui {
/// Main-thread callback; owner must outlive the outstanding request.
class BrowserQueryController::QueryCompletion {
public:
  QueryCompletion(BrowserQueryController& owner, std::size_t generation)
      : owner_(owner), generation_(generation) {}

  void operator()(application::AccessResult<application::Annotated<application::RecordMetadataList>>
                      result) const {
    owner_.request_active_ = false;
    if (generation_ != owner_.generation_) return;
    owner_.result_ = std::move(result);
    owner_.loading_ = false;
  }

private:
  BrowserQueryController& owner_;
  std::size_t generation_;
};

void BrowserQueryController::select(core::RecordType type) {
  ++generation_;
  loading_ = true;
  module_results_[static_cast<std::size_t>(selected_)] = std::move(result_);
  selected_ = type;
  result_ = std::move(module_results_[static_cast<std::size_t>(selected_)]);
  reload_pending_ = true;
  submit_pending();
}
void BrowserQueryController::invalidate() {
  ++generation_;
  reload_pending_ = true;
  loading_ = true;
  submit_pending();
}
void BrowserQueryController::submit_pending() {
  if (!reload_pending_ || request_active_) return;
  const auto generation = generation_;
  application::RecordQuery query;
  query.type = selected_;
  if (backend_.query_records(query, QueryCompletion{*this, generation})) {
    reload_pending_ = false;
    request_active_ = true;
  }
}
void BrowserQueryController::resume_pending() {
  submit_pending();
}
} // namespace folio::tui
