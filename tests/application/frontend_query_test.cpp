#include "tui/theme/color_scheme.hpp"
#include "tui/layouts/virtual_rows.hpp"
#include <algorithm>
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

TEST(TuiVirtualRowsTest, RenderingWorkDependsOnViewportRatherThanRecordCount) {
  using namespace ftxui;
  for (const int count : {10, 5000, 10000}) {
    for (const int height : {1, 12, 40}) {
      for (const int cursor : {0, count / 2, count - 1}) {
        std::vector<int> rendered;
        auto rows = std::make_shared<folio::tui::VirtualRows>(count, cursor, [&](int index) {
          rendered.push_back(index);
          return text(std::to_string(index));
        });
        auto layout = rows | vscroll_indicator | frame | flex;
        Screen screen(80, height);
        Render(screen, layout);
        EXPECT_LE(rendered.size(), static_cast<std::size_t>(height));
        EXPECT_NE(std::find(rendered.begin(), rendered.end(), cursor), rendered.end());
        for (int index : rendered) {
          EXPECT_GE(index, 0);
          EXPECT_LT(index, count);
        }
      }
    }
  }
}

TEST(TuiVirtualRowsTest, EmptyListDoesNotMaterializeRows) {
  auto rows = std::make_shared<folio::tui::VirtualRows>(0, 0, [](int) {
    ADD_FAILURE() << "Empty list requested a row";
    return ftxui::text("");
  });
  ftxui::Screen screen(80, 24);
  ftxui::Render(screen, rows | ftxui::frame | ftxui::flex);
}


TEST(TuiThemeTest, BuiltinsHaveCompleteSemanticColors) {
  {
    const auto* theme = folio::tui::find_theme("hweiluh-nocturne");
    ASSERT_NE(theme, nullptr);
    EXPECT_EQ(theme->scheme.primary(), ftxui::Color::RGB(0x58, 0xAE, 0xA4));
    EXPECT_EQ(theme->scheme.on_primary(), ftxui::Color::RGB(0x07, 0x17, 0x15));
    EXPECT_EQ(theme->scheme.primary_container(), ftxui::Color::RGB(0x29, 0x56, 0x51));
    EXPECT_EQ(theme->scheme.on_primary_container(), ftxui::Color::RGB(0xE0, 0xF2, 0xEB));
    EXPECT_EQ(theme->scheme.secondary(), ftxui::Color::RGB(0x9F, 0xAD, 0xC3));
    EXPECT_EQ(theme->scheme.on_secondary(), ftxui::Color::RGB(0x12, 0x1A, 0x24));
    EXPECT_EQ(theme->scheme.tertiary(), ftxui::Color::RGB(0xC4, 0x9B, 0x66));
    EXPECT_EQ(theme->scheme.on_tertiary(), ftxui::Color::RGB(0x24, 0x18, 0x0C));
    EXPECT_EQ(theme->scheme.surface(), ftxui::Color::RGB(0x11, 0x15, 0x16));
    EXPECT_EQ(theme->scheme.surface_container(), ftxui::Color::RGB(0x29, 0x33, 0x34));
    EXPECT_EQ(theme->scheme.surface_container_high(), ftxui::Color::RGB(0x35, 0x42, 0x44));
    EXPECT_EQ(theme->scheme.on_surface(), ftxui::Color::RGB(0xE7, 0xE7, 0xDC));
    EXPECT_EQ(theme->scheme.on_surface_variant(), ftxui::Color::RGB(0xA7, 0xB7, 0xB4));
    EXPECT_EQ(theme->scheme.outline(), ftxui::Color::RGB(0x63, 0x71, 0x6F));
    EXPECT_EQ(theme->scheme.outline_variant(), ftxui::Color::RGB(0x39, 0x47, 0x45));
    EXPECT_EQ(theme->scheme.focus(), ftxui::Color::RGB(0x58, 0xAE, 0xA4));
    EXPECT_EQ(theme->scheme.on_focus(), ftxui::Color::RGB(0x07, 0x17, 0x15));
    EXPECT_EQ(theme->scheme.text_muted(), ftxui::Color::RGB(0x8C, 0x99, 0x96));
    EXPECT_EQ(theme->scheme.success(), ftxui::Color::RGB(0x91, 0xBE, 0x98));
    EXPECT_EQ(theme->scheme.warning(), ftxui::Color::RGB(0xD5, 0xAF, 0x70));
    EXPECT_EQ(theme->scheme.error(), ftxui::Color::RGB(0xDE, 0x8E, 0x89));
    EXPECT_EQ(theme->scheme.info(), ftxui::Color::RGB(0x8E, 0xAE, 0xD4));
  }
  {
    const auto* theme = folio::tui::find_theme("gueidaang-afterglow");
    ASSERT_NE(theme, nullptr);
    EXPECT_EQ(theme->scheme.primary(), ftxui::Color::RGB(0xC7, 0xA4, 0x6C));
    EXPECT_EQ(theme->scheme.on_primary(), ftxui::Color::RGB(0x21, 0x18, 0x0C));
    EXPECT_EQ(theme->scheme.primary_container(), ftxui::Color::RGB(0x55, 0x45, 0x2E));
    EXPECT_EQ(theme->scheme.on_primary_container(), ftxui::Color::RGB(0xF0, 0xDF, 0xC0));
    EXPECT_EQ(theme->scheme.secondary(), ftxui::Color::RGB(0x8C, 0xA7, 0x9A));
    EXPECT_EQ(theme->scheme.on_secondary(), ftxui::Color::RGB(0x10, 0x1D, 0x17));
    EXPECT_EQ(theme->scheme.tertiary(), ftxui::Color::RGB(0xC5, 0x8E, 0x82));
    EXPECT_EQ(theme->scheme.on_tertiary(), ftxui::Color::RGB(0x2A, 0x11, 0x10));
    EXPECT_EQ(theme->scheme.surface(), ftxui::Color::RGB(0x1A, 0x19, 0x17));
    EXPECT_EQ(theme->scheme.surface_container(), ftxui::Color::RGB(0x33, 0x30, 0x2A));
    EXPECT_EQ(theme->scheme.surface_container_high(), ftxui::Color::RGB(0x45, 0x40, 0x37));
    EXPECT_EQ(theme->scheme.on_surface(), ftxui::Color::RGB(0xEB, 0xE2, 0xD1));
    EXPECT_EQ(theme->scheme.on_surface_variant(), ftxui::Color::RGB(0xBD, 0xB0, 0x9B));
    EXPECT_EQ(theme->scheme.outline(), ftxui::Color::RGB(0x75, 0x6B, 0x59));
    EXPECT_EQ(theme->scheme.outline_variant(), ftxui::Color::RGB(0x48, 0x42, 0x38));
    EXPECT_EQ(theme->scheme.focus(), ftxui::Color::RGB(0xD3, 0xB5, 0x7F));
    EXPECT_EQ(theme->scheme.on_focus(), ftxui::Color::RGB(0x21, 0x18, 0x0C));
    EXPECT_EQ(theme->scheme.text_muted(), ftxui::Color::RGB(0xA6, 0x9B, 0x8A));
    EXPECT_EQ(theme->scheme.success(), ftxui::Color::RGB(0x9E, 0xBC, 0x96));
    EXPECT_EQ(theme->scheme.warning(), ftxui::Color::RGB(0xD6, 0xB5, 0x7B));
    EXPECT_EQ(theme->scheme.error(), ftxui::Color::RGB(0xDE, 0x94, 0x8A));
    EXPECT_EQ(theme->scheme.info(), ftxui::Color::RGB(0x94, 0xAF, 0xC1));
  }
  EXPECT_EQ(&folio::tui::default_color_scheme(), &folio::tui::find_theme("hweiluh-nocturne")->scheme);
  EXPECT_EQ(folio::tui::find_theme("unknown"), nullptr);
  EXPECT_EQ(folio::tui::find_theme("Hweiluh-Nocturne"), nullptr);
}
