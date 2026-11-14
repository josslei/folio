#include "tui/components/loading_feedback.hpp"
namespace folio::tui {
LoadingFeedback::LoadingFeedback(std::function<void()> wake, std::chrono::milliseconds delay,
                                 std::chrono::milliseconds loaded_duration)
    : wake_(std::move(wake)), delay_(delay), loaded_duration_(loaded_duration),
      timer_([this](std::stop_token stop) { run(stop); }) {}
LoadingFeedback::~LoadingFeedback() {
  timer_.request_stop();
  changed_.notify_all();
  timer_.join();
}
void LoadingFeedback::begin(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  generation_ = generation;
  pending_ = true;
  visible_generation_.reset();
  deadline_ = Deadline{generation, std::chrono::steady_clock::now() + delay_};
  changed_.notify_all();
}
void LoadingFeedback::received(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  if (deadline_ && deadline_->generation == generation && !deadline_->hide) deadline_.reset();
  changed_.notify_all();
}
void LoadingFeedback::complete(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  if (generation_ == generation) pending_ = false;
  if (visible_generation_ != generation) return;
  if (loaded_duration_.count() <= 0)
    visible_generation_.reset();
  else
    deadline_ = Deadline{generation, std::chrono::steady_clock::now() + loaded_duration_, true};
  changed_.notify_all();
}
bool LoadingFeedback::visible(std::size_t generation) const {
  std::lock_guard lock(feedback_mutex_);
  return visible_generation_ == generation;
}
std::string LoadingFeedback::text(std::size_t generation, bool pending) const {
  if (!visible(generation)) return {};
  return pending ? "Loading..." : "Loaded";
}
ftxui::Element LoadingFeedback::OnRender() {
  return ftxui::text(text(generation_, pending_));
}
void LoadingFeedback::run(std::stop_token stop) {
  std::unique_lock lock(feedback_mutex_);
  while (!stop.stop_requested()) {
    if (!changed_.wait(lock, stop, [&] { return deadline_.has_value(); })) return;
    const auto next = *deadline_;
    if (changed_.wait_until(lock, stop, next.time, [&] {
          return !deadline_ || deadline_->generation != next.generation ||
                 deadline_->time != next.time;
        }))
      continue;
    if (stop.stop_requested()) return;
    if (next.hide)
      visible_generation_.reset();
    else
      visible_generation_ = next.generation;
    deadline_.reset();
    lock.unlock();
    wake_();
    lock.lock();
  }
}
} // namespace folio::tui
