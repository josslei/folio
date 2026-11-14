#include "tui/pages/browser/record_query.hpp"

namespace folio::tui {
void RecordQuery::select(core::RecordType type) {
  ++generation_;
  loading_ = true;
  module_results_[static_cast<std::size_t>(selected_)] = std::move(result_);
  selected_ = type;
  result_ = std::move(module_results_[static_cast<std::size_t>(selected_)]);
  reload_pending_ = true;
  submit_pending();
}
void RecordQuery::invalidate() {
  ++generation_;
  reload_pending_ = true;
  loading_ = true;
  submit_pending();
}
void RecordQuery::submit_pending() {
  if (!reload_pending_ || request_active_) return;
  const auto generation = generation_;
  application::RecordQuery query;
  query.type = selected_;
  if (backend_.query_records(query, [this, generation](QueryResult result) {
        request_active_ = false;
        if (generation == generation_) {
          result_ = std::move(result);
          loading_ = false;
        }
      })) {
    reload_pending_ = false;
    request_active_ = true;
  }
}
void RecordQuery::resume_pending() {
  submit_pending();
}
} // namespace folio::tui
