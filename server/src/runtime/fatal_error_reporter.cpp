#include "runtime/fatal_error_reporter.hpp"

namespace folio::application::internal {
void FatalErrorReporter::report(Failure error) {
  std::lock_guard lock(failures_mutex_);
  failures_.push_back(std::move(error));
  changed_.request_stop();
}
std::stop_token FatalErrorReporter::wake_token() {
  std::lock_guard lock(failures_mutex_);
  if (failures_.empty() && changed_.stop_requested()) changed_ = std::stop_source{};
  return changed_.get_token();
}
std::optional<FatalErrorReporter::Failure> FatalErrorReporter::take_one() {
  std::lock_guard lock(failures_mutex_);
  if (failures_.empty()) return {};
  auto error = std::move(failures_.front());
  failures_.erase(failures_.begin());
  return error;
}
std::vector<FatalErrorReporter::Failure> FatalErrorReporter::take_failures() {
  std::lock_guard lock(failures_mutex_);
  std::vector<Failure> result;
  result.swap(failures_);
  return result;
}
} // namespace folio::application::internal
