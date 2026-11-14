#include "tui/components/loading_status.hpp"
namespace folio::tui {
LoadingStatus::LoadingStatus(std::function<void()> wake, std::chrono::milliseconds delay,
                                 std::chrono::milliseconds loaded_duration)
    : wake_(std::move(wake)), delay_(delay), loaded_duration_(loaded_duration),
      timer_([this](std::stop_token stop) { run(stop); }) {}
LoadingStatus::~LoadingStatus() {
  timer_.request_stop();
  changed_.notify_all();
  timer_.join();
}
void LoadingStatus::begin(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  generation_ = generation;
  pending_ = true;
  visible_generation_.reset();
  loading_shown_at_.reset();
  deadline_ = Deadline{generation, std::chrono::steady_clock::now() + delay_};
  changed_.notify_all();
}
void LoadingStatus::received(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  if (deadline_ && deadline_->generation == generation &&
      deadline_->action == Deadline::Action::ShowLoading) deadline_.reset();
  changed_.notify_all();
}
void LoadingStatus::complete(std::size_t generation) {
  std::lock_guard lock(feedback_mutex_);
  if (generation_ == generation) pending_ = false;
  if (visible_generation_ != generation) return;
  const auto now = std::chrono::steady_clock::now();
  const auto loading_until = loading_shown_at_.value_or(now) + std::chrono::milliseconds(100);
  if (loading_shown_at_ && now < loading_until)
    deadline_ = Deadline{generation, loading_until, Deadline::Action::ShowLoaded};
  else if (loaded_duration_.count() <= 0)
    visible_generation_.reset();
  else
    deadline_ = Deadline{generation, now + loaded_duration_, Deadline::Action::Hide};
  changed_.notify_all();
}
bool LoadingStatus::visible(std::size_t generation) const {
  std::lock_guard lock(feedback_mutex_);
  return visible_generation_ == generation;
}
std::string LoadingStatus::text(std::size_t generation, bool pending) const {
  std::lock_guard lock(feedback_mutex_);
  if (visible_generation_ != generation) return "Ready";
  if (pending || (deadline_ && deadline_->action == Deadline::Action::ShowLoaded)) {
    if (!loading_shown_at_) loading_shown_at_ = std::chrono::steady_clock::now();
    return "Loading...";
  }
  return "Loaded";
}
ftxui::Element LoadingStatus::OnRender() {
  return ftxui::text(text(generation_, pending_));
}
void LoadingStatus::run(std::stop_token stop) {
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
    if (next.action == Deadline::Action::Hide)
      visible_generation_.reset();
    else if (next.action == Deadline::Action::ShowLoading)
      visible_generation_ = next.generation;
    deadline_.reset();
    if (next.action == Deadline::Action::ShowLoaded) {
      if (loaded_duration_.count() <= 0)
        visible_generation_.reset();
      else
        deadline_ = Deadline{next.generation, std::chrono::steady_clock::now() + loaded_duration_,
                             Deadline::Action::Hide};
    }
    lock.unlock();
    wake_();
    lock.lock();
  }
}
} // namespace folio::tui
