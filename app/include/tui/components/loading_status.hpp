#pragma once
#include <ftxui/component/component_base.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <string>

namespace folio::tui {
/// TUI Ready / Loading / Loaded presentation timing. Control methods run on the UI
/// thread; the private timer only wakes the event loop. Wake must be
/// thread-safe and outlive this object. No polling or result delay.
class LoadingStatus : public ftxui::ComponentBase {
public:
  LoadingStatus(std::function<void()> wake, std::chrono::milliseconds delay,
                  std::chrono::milliseconds loaded_duration);
  ~LoadingStatus();
  void begin(std::size_t generation);
  /// Cancel an unseen Loading deadline when the Backend result arrives.
  void received(std::size_t generation);
  /// Transition to Loaded after the minimum Loading display time.
  void complete(std::size_t generation);
  bool visible(std::size_t generation) const;
  /// Render the feedback in the Bottom Bar without changing its geometry.
  ftxui::Element OnRender() override;
  /// Presentation text defaults to Ready when no timed feedback is visible.
  std::string text(std::size_t generation, bool pending) const;

private:
  std::size_t generation_ = 0;
  bool pending_ = false;
  struct Deadline {
    std::size_t generation;
    std::chrono::steady_clock::time_point time;
    enum class Action { ShowLoading, ShowLoaded, Hide } action = Action::ShowLoading;
  };
  void run(std::stop_token stop);
  std::function<void()> wake_;
  std::chrono::milliseconds delay_;
  std::chrono::milliseconds loaded_duration_;
  // Protects deadline and visible generation; never held while invoking wake.
  mutable std::mutex feedback_mutex_;
  std::condition_variable_any changed_;
  std::optional<Deadline> deadline_;
  std::optional<std::size_t> visible_generation_;
  mutable std::optional<std::chrono::steady_clock::time_point> loading_shown_at_;
  std::jthread timer_;
};
} // namespace folio::tui
