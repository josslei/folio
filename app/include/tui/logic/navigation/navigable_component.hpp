#pragma once
#include "tui/logic/navigation/navigation.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <tuple>
#include <type_traits>
namespace folio::tui::navigation {
/// Owner-local compile-time child declaration; not live objects or a global tree.
template <class... UI> using Children = std::tuple<std::type_identity<UI>...>;
/// Check a child against its owner's explicit NavigationChildren declaration.
template <class Owner, class Child> constexpr bool declares_child() {
  return std::apply(
      [](auto... children) {
        return (std::same_as<Child, typename decltype(children)::type> || ...);
      },
      typename Owner::NavigationChildren{});
}

/// Shared FTXUI/navigation integration base. Generates static relationships from
/// each UI class's declarations and binds its session factories in a local copy.
/// It does not own Current Location or History; those belong to Manager.
class NavigableComponent : public ftxui::ComponentBase, public INavigable {
public:
  explicit NavigableComponent(const Description& description) : description_(description) {}
  const Description& description() const { return description_; }
  const Description* static_description() const override { return &description_; }
  bool visitable(const Context&) const override { return description_.visitable; }
  std::vector<Target> children(const Context& context) const override {
    std::vector<Target> result;
    result.reserve(description_.children.size());
    for (const auto& child : description_.children)
      result.push_back(INavigable::target(child().anchor, child().create, context));
    return result;
  }
  std::vector<Target> cross_targets(const Context&) const override { return {}; }
  std::shared_ptr<INavigable> resolve_child(const Anchor&, const Context&) const override {
    return {};
  }

protected:
  using INavigable::target;
  /// Static issuance is constrained by the owner's declared relationships. The
  /// owner argument binds this shared helper to the concrete class registration.
  template <class Owner, class Child>
    requires(declares_child<Owner, Child>())
  Target target(const Context& context) const {
    if (description_.anchor != Anchor::of<Owner>())
      throw std::logic_error("Navigation target helper bound to a different owner");
    constexpr auto index = []<std::size_t... I>(std::index_sequence<I...>) {
      return ((std::same_as<std::type_identity<Child>,
                            std::tuple_element_t<I, typename Owner::NavigationChildren>>
                   ? I
                   : 0) +
              ...);
    }(std::make_index_sequence<std::tuple_size_v<typename Owner::NavigationChildren>>{});
    const auto& child = description_.children[index]();
    return INavigable::target(child.anchor, child.create, context);
  }

  /// Bind session services without exposing their types to navigation. The
  /// callable must outlive temporary UI instances and must not capture this.
  template <class Owner, class Child>
    requires(declares_child<Owner, Child>())
  void bind_child(Factory factory) {
    if (description_.anchor != Anchor::of<Owner>())
      throw std::logic_error("Navigation factory bound to a different owner");
    for (auto& getter : description_.children) {
      if (getter().anchor != Anchor::of<Child>()) continue;
      auto child = std::make_shared<Description>(getter());
      child->create = std::move(factory);
      getter = [child]() -> const Description& { return *child; };
      return;
    }
  }

public:
  template <class UI> static Description describe() {
    Description result{Anchor::registered<UI>(), true, {}};
    result.children.reserve(std::tuple_size_v<typename UI::NavigationChildren>);
    if constexpr (requires { UI::navigation_visitable; })
      result.visitable = UI::navigation_visitable;
    std::apply(
        [&](auto... child) {
          (result.children.push_back(
               &folio::tui::navigation::description<typename decltype(child)::type>),
           ...);
        },
        typename UI::NavigationChildren{});
    return result;
  }

private:
  Description description_;
};
} // namespace folio::tui::navigation
