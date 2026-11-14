#include "tui/logic/browser/browser_query_controller.hpp"
#include "tui/pages/browser/browser_page.hpp"
#include "tui/shell/tui_shell.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>
#include <future>

TEST(TuiLoadingStatusTest, NavigationResetsOldIndicator) {
  using namespace folio;
  std::mutex mutex;
  std::condition_variable changed;
  int wakes = 0;
  tui::LoadingStatus feedback(
      [&] {
        std::lock_guard lock(mutex);
        ++wakes;
        changed.notify_all();
      },
      std::chrono::milliseconds(0), std::chrono::milliseconds(0));
  feedback.begin(1);
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return wakes >= 1; });
  }
  EXPECT_EQ(feedback.text(1, true), "Loading...");
  feedback.begin(2);
  EXPECT_FALSE(feedback.visible(1));
  feedback.received(2);
  feedback.complete(2);
  EXPECT_EQ(feedback.text(1, false), "Ready");
}

TEST(TuiLoadingStatusTest, LoadedHasItsOwnDuration) {
  using namespace folio;
  std::mutex mutex;
  std::condition_variable changed;
  int wakes = 0;
  tui::LoadingStatus feedback(
      [&] {
        std::lock_guard lock(mutex);
        ++wakes;
        changed.notify_all();
      },
      std::chrono::milliseconds(0), std::chrono::milliseconds(100));
  feedback.begin(1);
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return wakes >= 1; });
  }
  EXPECT_EQ(feedback.text(1, true), "Loading...");
  feedback.received(1);
  feedback.complete(1);
  EXPECT_EQ(feedback.text(1, false), "Loading...");
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return wakes >= 2; }));
  }
  EXPECT_EQ(feedback.text(1, false), "Loaded");
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return wakes >= 3; }));
  }
  EXPECT_EQ(feedback.text(1, false), "Ready");
}

TEST(TuiLoadingStatusTest, FastCompletionDoesNotShowFeedback) {
  folio::tui::LoadingStatus feedback([] {}, std::chrono::hours(1), std::chrono::seconds(1));
  EXPECT_EQ(feedback.text(0, false), "Ready");
  feedback.begin(1);
  EXPECT_EQ(feedback.text(1, true), "Ready");
  feedback.received(1);
  feedback.complete(1);
  EXPECT_EQ(feedback.text(1, false), "Ready");
}

TEST(TuiShellTest, QuitIsGlobalAndPlainQDoesNotExit) {
  bool quit = false;
  auto page = ftxui::Renderer([] { return ftxui::text("Page content"); });
  folio::tui::TuiShell shell(page, [] { return ftxui::text("Feedback"); }, [&] { quit = true; });
  shell.OnEvent(ftxui::Event::Character('q'));
  EXPECT_FALSE(quit);
  EXPECT_TRUE(shell.OnEvent(ftxui::Event::CtrlQ));
  EXPECT_TRUE(quit);
  ftxui::Screen screen(80, 24);
  ftxui::Render(screen, shell.Render());
  EXPECT_NE(screen.ToString().find("Page content"), std::string::npos);
  EXPECT_NE(screen.ToString().find("Feedback"), std::string::npos);
  EXPECT_NE(screen.ToString().find("C-q Quit"), std::string::npos);
  EXPECT_EQ(screen.CellAt(0, 22).background_color, ftxui::Color::White);
  EXPECT_EQ(screen.CellAt(79, 22).background_color, ftxui::Color::White);
  EXPECT_EQ(screen.CellAt(0, 0).background_color, ftxui::Color::Default);
  EXPECT_EQ(screen.CellAt(0, 23).background_color, ftxui::Color::Default);
}

TEST(TuiTopBarTest, MovingCursorDoesNotActivateUntilEnter) {
  using namespace folio;
  int calls = 0;
  core::RecordType selected = core::RecordType::Log;
  auto bar = std::make_shared<tui::TopBar>([&](core::RecordType type) {
    ++calls;
    selected = type;
  });
  EXPECT_TRUE(bar->OnEvent(ftxui::Event::ArrowRight));
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(bar->OnEvent(ftxui::Event::Return));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(selected, core::RecordType::Memo);
}

TEST(TuiTopBarTest, CursorAndSelectionHaveDistinctStyles) {
  auto bar = std::make_shared<folio::tui::TopBar>([](folio::core::RecordType) {});
  auto page = ftxui::Container::Vertical({bar});
  folio::tui::TuiShell shell(page, [] { return ftxui::text(""); }, [] {});
  ftxui::Screen initial(80, 5);
  ftxui::Render(initial, shell.Render());
  EXPECT_EQ(initial.CellAt(1, 1).character, "[");
  EXPECT_FALSE(initial.CellAt(4, 1).underlined);
  EXPECT_EQ(initial.CellAt(4, 1).background_color, ftxui::Color::White);

  ASSERT_TRUE(bar->OnEvent(ftxui::Event::ArrowRight));
  ftxui::Screen moved(80, 5);
  ftxui::Render(moved, shell.Render());
  EXPECT_EQ(moved.CellAt(1, 1).character, " ");
  EXPECT_EQ(moved.CellAt(4, 1).background_color, ftxui::Color::White);
  EXPECT_EQ(moved.CellAt(11, 1).character, " ");
  EXPECT_EQ(moved.CellAt(17, 1).character, "[");
  EXPECT_FALSE(moved.CellAt(23, 1).underlined);
  EXPECT_EQ(moved.CellAt(4, 2).character, "▔");
  EXPECT_EQ(moved.CellAt(23, 2).character, "▔");
  EXPECT_NE(moved.CellAt(23, 1).background_color, ftxui::Color::White);
}
