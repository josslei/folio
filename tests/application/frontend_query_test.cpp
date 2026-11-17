#include "tui/theme/color_scheme.hpp"
#include "tui/layouts/virtual_rows.hpp"
#include <algorithm>
#include "tui/logic/browser/browser_query_controller.hpp"
#include "tui/logic/navigation/virtual_subtree.hpp"
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
  EXPECT_EQ(screen.CellAt(0, 22).background_color,
            folio::tui::default_color_scheme().surface_container());
  EXPECT_EQ(screen.CellAt(79, 22).background_color,
            folio::tui::default_color_scheme().surface_container());
  EXPECT_EQ(screen.CellAt(0, 0).background_color, folio::tui::default_color_scheme().surface());
  EXPECT_EQ(screen.CellAt(0, 23).background_color, folio::tui::default_color_scheme().surface());
}

TEST(TuiTopBarTest, MovingCursorDoesNotActivateUntilEnter) {
  using namespace folio;
  int calls = 0;
  tui::BrowserKind selected = tui::BrowserKind::Logs;
  auto bar = std::make_shared<tui::TopBar>(
      [&](tui::BrowserKind type) {
        ++calls;
        selected = type;
      },
      [&] { return selected; });
  EXPECT_TRUE(bar->OnEvent(ftxui::Event::ArrowRight));
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(bar->OnEvent(ftxui::Event::Return));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(selected, tui::BrowserKind::Memos);
}

TEST(TuiTopBarTest, CursorAndSelectionHaveDistinctStyles) {
  auto bar = std::make_shared<folio::tui::TopBar>([](folio::tui::BrowserKind) {},
                                                  [] { return folio::tui::BrowserKind::Logs; });
  auto page = ftxui::Container::Vertical({bar});
  folio::tui::TuiShell shell(page, [] { return ftxui::text(""); }, [] {});
  ftxui::Screen initial(80, 5);
  ftxui::Render(initial, shell.Render());
  EXPECT_EQ(initial.CellAt(1, 1).character, "[");
  EXPECT_FALSE(initial.CellAt(4, 1).underlined);
  EXPECT_EQ(initial.CellAt(4, 1).background_color,
            folio::tui::default_color_scheme().primary_container());

  ASSERT_TRUE(bar->OnEvent(ftxui::Event::ArrowRight));
  ftxui::Screen moved(80, 5);
  ftxui::Render(moved, shell.Render());
  EXPECT_EQ(moved.CellAt(1, 1).character, " ");
  EXPECT_EQ(moved.CellAt(4, 1).background_color,
            folio::tui::default_color_scheme().primary_container());
  EXPECT_EQ(moved.CellAt(11, 1).character, " ");
  EXPECT_EQ(moved.CellAt(17, 1).character, "[");
  EXPECT_FALSE(moved.CellAt(23, 1).underlined);
  EXPECT_EQ(moved.CellAt(4, 2).character, "▔");
  EXPECT_EQ(moved.CellAt(23, 2).character, "▔");
  EXPECT_NE(moved.CellAt(23, 1).background_color,
            folio::tui::default_color_scheme().primary_container());
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
  EXPECT_EQ(&folio::tui::default_color_scheme(),
            &folio::tui::find_theme("hweiluh-nocturne")->scheme);
  EXPECT_EQ(folio::tui::find_theme("unknown"), nullptr);
  EXPECT_EQ(folio::tui::find_theme("Hweiluh-Nocturne"), nullptr);
}

namespace {
using namespace folio::tui;
static_assert(std::is_abstract_v<navigation::INavigable>);
enum class TestChild { First, Second, Third, Nested };
enum class OtherChild { First };
class NavigationLeaf final : public navigation::INavigable {
public:
  std::vector<navigation::Target> children(const navigation::Context&) const override { return {}; }
  std::vector<navigation::Target> cross_targets(const navigation::Context&) const override {
    return {};
  }
  std::shared_ptr<navigation::INavigable> resolve_child(const navigation::Anchor&,
                                                        const navigation::Context&) const override {
    return {};
  }
};
// One dynamic owner declares its positions through the same target issuance API.
class NavigationOwner final : public navigation::INavigable {
public:
  mutable std::shared_ptr<NavigationLeaf> leaf = std::make_shared<NavigationLeaf>();
  mutable int enumerations = 0;
  bool fail_lookup = false;
  mutable bool release_during_lookup = false;
  navigation::Target declared(TestChild value, const navigation::Context& context) const {
    if (value == TestChild::Nested) throw std::logic_error("Undeclared child");
    return target(navigation::Anchor{value}, context);
  }
  navigation::Target declared_cross(TestChild value, navigation::CrossMode mode,
                                    const navigation::Context& context) const {
    return target(navigation::Location{navigation::Anchor{value}}, mode, context);
  }
  navigation::Target declared_cross(navigation::Location destination, navigation::CrossMode mode,
                                    const navigation::Context& context) const {
    return target(std::move(destination), mode, context);
  }
  std::vector<navigation::Target> children(const navigation::Context& context) const override {
    ++enumerations;
    return {declared(TestChild::First, context), declared(TestChild::Second, context),
            declared(TestChild::Third, context)};
  }
  std::vector<navigation::Target> cross_targets(const navigation::Context& context) const override {
    ++enumerations;
    return {declared_cross(TestChild::Second, navigation::CrossMode::Push, context),
            declared_cross(TestChild::Second, navigation::CrossMode::Switch, context)};
  }
  std::shared_ptr<navigation::INavigable> resolve_child(const navigation::Anchor& child,
                                                        const navigation::Context&) const override {
    if (fail_lookup) throw std::runtime_error("Lookup failure");
    if (!child.get_if<TestChild>()) return {};
    auto retained = leaf;
    if (release_during_lookup) leaf.reset();
    return retained;
  }
};
} // namespace

TEST(TuiNavigationTest, AnchorsPreserveOwnerTypesAndValues) {
  class FirstUI;
  class SecondUI;
  EXPECT_EQ(navigation::Anchor::of<FirstUI>(), navigation::Anchor::of<FirstUI>());
  EXPECT_NE(navigation::Anchor::of<FirstUI>(), navigation::Anchor::of<SecondUI>());
  EXPECT_NE(navigation::Anchor{TestChild::First}, navigation::Anchor{OtherChild::First});
  EXPECT_NE(navigation::Anchor{TestChild::First}, navigation::Anchor{TestChild::Second});
}

namespace {
// Synthetic semantic positions exercise the generic capability, not a second
// production Page hierarchy. No UI instance is allocated per position.
struct VirtualPosition {
  int value;
  bool operator==(const VirtualPosition&) const = default;
};
class VirtualOwner {
public:
  navigation::VirtualSubtree<VirtualPosition> subtree;
  VirtualOwner()
      : subtree([] { return create(); },
                [](const auto& subtree, const navigation::Context& context) {
                  return std::vector<navigation::Target>{
                      subtree.child({subtree.position(context).value + 1}, context)};
                }) {}
  static std::shared_ptr<navigation::INavigable> create() {
    return navigation::VirtualSubtree<VirtualPosition>::retained(std::make_shared<VirtualOwner>(),
                                                                 &VirtualOwner::subtree);
  }
};
class VirtualHost : public navigation::INavigable {
public:
  std::shared_ptr<navigation::INavigable> mounted;
  std::vector<navigation::Target> children(const navigation::Context&) const override { return {}; }
  std::vector<navigation::Target> cross_targets(const navigation::Context&) const override {
    return {};
  }
  std::shared_ptr<navigation::INavigable>
  resolve_child(const navigation::Anchor& value,
                const navigation::Context& context) const override {
    return mounted && mounted->represents(value, context) ? mounted : nullptr;
  }
  void host(const std::shared_ptr<navigation::INavigable>& prepared,
            const navigation::Context&) override {
    mounted = prepared;
  }
};
} // namespace

TEST(TuiNavigationTest, VirtualSubtreeSharesOneOwnerAcrossPositionsAndHistory) {
  VirtualHost host;
  const navigation::Location root{
      navigation::Anchor{VirtualPosition{0}}.with_factory(&VirtualOwner::create)};
  navigation::Manager manager(host, root);
  auto* subtree = dynamic_cast<navigation::VirtualSubtree<VirtualPosition>*>(host.mounted.get());
  ASSERT_NE(subtree, nullptr);
  auto initial_object = host.mounted;
  auto children = subtree->children({manager.current_location(), 1});
  ASSERT_EQ(children.size(), 1u);
  EXPECT_EQ(manager.down(children.front()), navigation::Result::Changed);
  EXPECT_EQ(host.mounted, initial_object);
  EXPECT_EQ(subtree->position({manager.current_location(), 2}).value, 1);
  EXPECT_EQ(subtree->position({root, 1}).value, 0); // Old Context is independent.
  EXPECT_EQ(manager.history().size(), 1u);
  const auto source = manager.current_location();
  auto destination = root;
  destination.back() = subtree->anchor({7});
  EXPECT_EQ(manager.cross(
                subtree->cross(destination, navigation::CrossMode::Push, {source, source.size()})),
            navigation::Result::Changed);
  EXPECT_EQ(manager.history_back(), navigation::Result::Changed);
  EXPECT_EQ(manager.current_location(), source);
  EXPECT_EQ(manager.up(), navigation::Result::Changed);
  EXPECT_EQ(manager.current_location(), root);
}

TEST(TuiNavigationTest, VirtualSubtreeAliasPinsOwnerWithoutCycleAndPathReconstructs) {
  auto owner = std::make_shared<VirtualOwner>();
  std::weak_ptr<VirtualOwner> weak = owner;
  auto alias = navigation::VirtualSubtree<VirtualPosition>::retained(owner, &VirtualOwner::subtree);
  const navigation::Location saved{owner->subtree.anchor({42})};
  owner.reset();
  EXPECT_FALSE(weak.expired());
  alias.reset();
  EXPECT_TRUE(weak.expired());
  VirtualHost host;
  navigation::Manager manager(host, saved);
  ASSERT_NE(host.mounted, nullptr);
  EXPECT_EQ(manager.current_location(), saved);
  EXPECT_TRUE(host.mounted->represents(saved.back(), {saved, 1}));
  EXPECT_FALSE(host.mounted->represents(navigation::Anchor{42}, {saved, 1}));
}

TEST(TuiNavigationTest, StaticTargetsRequireDeclaredChildAtCompileTime) {
  using namespace folio::tui;
  static_assert(navigation::declares_child<BrowserPage, TopBar>());
  static_assert(!navigation::declares_child<TopBar, BrowserPage>());
  static_assert(!std::is_constructible_v<navigation::Target, navigation::Anchor>);
  static_assert(!std::is_invocable_v<decltype(&navigation::Manager::down), navigation::Manager&,
                                     navigation::Anchor>);
}

TEST(TuiNavigationTest, SwitchKeepsHistoryStartWithoutFalseBackAndPushSavesActualSource) {
  NavigationOwner owner;
  const navigation::Location anchor{navigation::Anchor{TestChild::First}};
  navigation::Manager manager(owner, anchor);
  auto cross = [&](TestChild value, navigation::CrossMode mode) {
    return owner.declared_cross(value, mode, {manager.current_location(), 0});
  };
  EXPECT_EQ(manager.cross(cross(TestChild::Second, navigation::CrossMode::Switch)),
            navigation::Result::Changed);
  EXPECT_EQ(manager.history().size(), 1u);
  EXPECT_EQ(manager.history_start(), anchor);
  EXPECT_EQ(manager.history().front(), manager.current_location());
  EXPECT_EQ(manager.history_back(), navigation::Result::Unchanged);
  const auto source = manager.current_location();
  EXPECT_EQ(manager.cross(cross(TestChild::Third, navigation::CrossMode::Push)),
            navigation::Result::Changed);
  const auto destination = manager.current_location();
  EXPECT_EQ(manager.history_back(), navigation::Result::Changed);
  EXPECT_EQ(manager.current_location(), source);
  EXPECT_EQ(manager.history_forward(), navigation::Result::Changed);
  EXPECT_EQ(manager.current_location(), destination);
  EXPECT_EQ(manager.history()[manager.history_cursor()], manager.current_location());
  EXPECT_EQ(owner.enumerations, 0); // Manager trusts issued capabilities.
  EXPECT_EQ(manager.history_back(), navigation::Result::Changed);
  manager.remember_position(17);
  EXPECT_EQ(manager.history_forward(), navigation::Result::Changed); // Focus preserves Forward.
  EXPECT_EQ(manager.history_back(), navigation::Result::Changed);
  manager.cross(cross(TestChild::First, navigation::CrossMode::Switch));
  EXPECT_EQ(manager.history_forward(), navigation::Result::Unchanged);
  EXPECT_EQ(manager.history_start(), anchor);
  EXPECT_EQ(manager.history()[manager.history_cursor()], manager.current_location());
}

TEST(TuiNavigationTest, CrossActionsToSameDestinationExecuteTheirDeclaredModes) {
  NavigationOwner owner;
  const navigation::Location anchor{navigation::Anchor{TestChild::First}};
  navigation::Manager pushed(owner, anchor);
  navigation::Manager switched(owner, anchor);
  const auto actions = owner.cross_targets({anchor, 0});
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].destination(), actions[1].destination());
  ASSERT_EQ(pushed.cross(actions[0]), navigation::Result::Changed);
  ASSERT_EQ(switched.cross(actions[1]), navigation::Result::Changed);
  EXPECT_EQ(pushed.current_location(), switched.current_location());
  EXPECT_EQ(pushed.history_back(), navigation::Result::Changed);
  EXPECT_EQ(pushed.current_location(), anchor);
  EXPECT_EQ(switched.history_back(), navigation::Result::Unchanged);
}

TEST(TuiNavigationTest, ResolutionFailureDoesNotCommitNavigation) {
  NavigationOwner owner;
  navigation::Manager manager(owner, {navigation::Anchor{TestChild::First}});
  auto target = owner.declared_cross(TestChild::Second, navigation::CrossMode::Push,
                                     {manager.current_location(), 0});
  const auto initial = manager.current_location();
  const auto history = manager.history();
  const auto cursor = manager.history_cursor();
  owner.fail_lookup = true;
  EXPECT_THROW(manager.cross(target), std::runtime_error);
  EXPECT_EQ(manager.current_location(), initial);
  EXPECT_EQ(manager.history().size(), 1u);
  EXPECT_EQ(manager.history(), history);
  EXPECT_EQ(manager.history_cursor(), cursor);
  owner.fail_lookup = false;
  EXPECT_EQ(manager.cross(target), navigation::Result::Changed);
  EXPECT_THROW(manager.cross(target), std::logic_error); // Stale source capability.
}

TEST(TuiNavigationTest, StructuralDownClearsForwardWithoutAddingHistoryAndUpUsesSavedAncestry) {
  class Parent final : public navigation::INavigable {
  public:
    std::shared_ptr<NavigationOwner> child = std::make_shared<NavigationOwner>();
    mutable int resolutions = 0;
    std::vector<navigation::Target> children(const navigation::Context& context) const override {
      return {target(navigation::Anchor{OtherChild::First}, context)};
    }
    std::vector<navigation::Target> cross_targets(const navigation::Context&) const override {
      return {};
    }
    std::shared_ptr<navigation::INavigable>
    resolve_child(const navigation::Anchor&, const navigation::Context&) const override {
      ++resolutions;
      return child;
    }
  } parent;
  navigation::Manager manager(parent, {navigation::Anchor{OtherChild::First}});
  const auto& anchor = manager.current_location();
  manager.down(parent.child->declared(TestChild::First, {anchor, anchor.size()}));
  const auto first = manager.current_location();
  auto destination = first;
  destination.back() = navigation::Anchor{TestChild::Second};
  manager.cross(parent.child->declared_cross(destination, navigation::CrossMode::Push,
                                             {manager.current_location(), 1}));
  ASSERT_EQ(manager.history_back(), navigation::Result::Changed);
  const auto size_before = manager.history().size();
  EXPECT_EQ(manager.up(), navigation::Result::Changed);
  EXPECT_EQ(manager.history()[manager.history_cursor()], manager.current_location());
  EXPECT_EQ(parent.resolutions, 1);
  EXPECT_EQ(manager.history().size(), size_before - 1); // Forward was discarded.
  EXPECT_EQ(manager.history_forward(), navigation::Result::Unchanged);
  const auto size_after = manager.history().size();
  manager.remember_position(42);
  EXPECT_EQ(manager.history().size(), size_after); // Focus/cursor snapshots add no steps.
  EXPECT_EQ(std::any_cast<int>(manager.restored_position()), 42);
  const auto snapshot = manager.restored_position();
  manager.remember_position(99);
  for (int i = 0; i < 128; ++i)
    manager.remember_position({navigation::Anchor{i}}, i);
  EXPECT_EQ(std::any_cast<int>(snapshot), 42);
  EXPECT_EQ(std::any_cast<int>(manager.restored_position()), 99);
  manager.cross(parent.child->declared_cross(destination, navigation::CrossMode::Push,
                                             {manager.current_location(), 1}));
  ASSERT_EQ(manager.history_back(), navigation::Result::Changed);
  manager.down(parent.child->declared(TestChild::Third,
                                       {manager.current_location(), manager.current_location().size()}));
  EXPECT_EQ(manager.history_forward(), navigation::Result::Unchanged);
  EXPECT_EQ(manager.history()[manager.history_cursor()], manager.current_location());
  EXPECT_EQ(parent.child->enumerations, 0);
}

TEST(TuiNavigationTest, ResolvedObjectSurvivesOwnerReleaseAndCacheDoesNotOwnIt) {
  NavigationOwner owner;
  navigation::Manager manager(owner, {navigation::Anchor{TestChild::First}});
  std::weak_ptr<navigation::INavigable> lifetime = owner.leaf;
  auto retained =
      owner.resolve_child(navigation::Anchor{TestChild::First}, {manager.current_location(), 0});
  owner.leaf.reset();
  EXPECT_FALSE(lifetime.expired());
  retained.reset();
  EXPECT_TRUE(lifetime.expired());
  owner.leaf = std::make_shared<NavigationLeaf>();
  bool alive_at_commit = false;
  navigation::Manager second(
      owner, {navigation::Anchor{TestChild::First}},
      [&](const navigation::Location&, const std::any&) { alive_at_commit = !lifetime.expired(); });
  lifetime = owner.leaf;
  owner.release_during_lookup = true;
  second.cross(owner.declared_cross(TestChild::Second, navigation::CrossMode::Switch,
                                    {second.current_location(), 0}));
  EXPECT_TRUE(alive_at_commit);
  EXPECT_TRUE(lifetime.expired());
}

TEST(TuiNavigationTest, OneObjectResolvesMultipleLogicalPositionsWithoutCurrentNodeState) {
  class Positions final : public navigation::INavigable {
  public:
    std::weak_ptr<navigation::INavigable> self;
    std::vector<navigation::Target> children(const navigation::Context& context) const override {
      return {target(navigation::Anchor{TestChild::Second}, context),
              target(navigation::Anchor{TestChild::Third}, context)};
    }
    std::vector<navigation::Target> cross_targets(const navigation::Context&) const override {
      return {};
    }
    std::shared_ptr<navigation::INavigable>
    resolve_child(const navigation::Anchor&, const navigation::Context&) const override {
      return self.lock();
    }
  };
  auto positions = std::make_shared<Positions>();
  positions->self = positions;
  navigation::Manager manager(*positions, {navigation::Anchor{TestChild::First}});
  auto targets =
      positions->children({manager.current_location(), manager.current_location().size()});
  EXPECT_EQ(manager.down(targets.front()), navigation::Result::Changed);
  EXPECT_EQ(manager.up(), navigation::Result::Changed);
  EXPECT_EQ(manager.down(targets.back()), navigation::Result::Changed);
  EXPECT_EQ(manager.history().size(), 1u);
}
