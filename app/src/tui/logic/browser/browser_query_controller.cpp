#include "tui/logic/browser/browser_query_controller.hpp"

namespace folio::tui {
/// Main-thread callback; locking pins a live controller only during delivery.
class BrowserQueryController::QueryCompletion {
public:
  QueryCompletion(std::weak_ptr<BrowserQueryController> owner, std::size_t generation)
      : owner_(std::move(owner)), generation_(generation) {}

  void operator()(application::AccessResult<application::Annotated<application::RecordMetadataList>>
                      result) const {
    auto owner = owner_.lock();
    if (!owner) return;
    owner->request_active_ = false;
    if (generation_ != owner->generation_) return;
    owner->result_ = std::move(result);
    owner->loading_ = false;
  }

private:
  std::weak_ptr<BrowserQueryController> owner_;
  std::size_t generation_;
};

void BrowserQueryController::select(core::RecordType type) {
  ++generation_;
  loading_ = true;
  module_results_[static_cast<std::size_t>(query_type_)] = std::move(result_);
  query_type_ = type;
  result_ = std::move(module_results_[static_cast<std::size_t>(query_type_)]);
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
  query.type = query_type_;
  if (backend_.query_records(query, QueryCompletion{shared_from_this(), generation})) {
    reload_pending_ = false;
    request_active_ = true;
  }
}
void BrowserQueryController::resume_pending() {
  submit_pending();
}
} // namespace folio::tui
