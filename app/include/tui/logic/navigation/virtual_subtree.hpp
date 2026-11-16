#pragma once

#include "tui/logic/navigation/navigation.hpp"

namespace folio::tui::navigation {
/// A nonvisual navigation capability owned as a member of a UI component.
/// One instance interprets many Position values through Context; it stores no
/// current position, materialized tree, or per-position objects. Position should
/// be an owner-specific value type, so unrelated subtrees cannot confuse IDs.
template <class Position>
  requires std::copy_constructible<Position> && std::equality_comparable<Position>
class VirtualSubtree final : public INavigable {
public:
  /// Owner-defined relationships evaluated only when requested, not on rendering.
  /// Call child()/cross() here to issue concrete owner-declared actions. Empty
  /// callbacks mean no actions. Captures may borrow the owning UI: retained()
  /// pins that UI whenever Manager accesses this member.
  using Targets = std::function<std::vector<Target>(const VirtualSubtree&, const Context&)>;
  /// Local, non-blocking semantic checks; must not perform synchronous Backend I/O.
  using Visitable = std::function<bool(const Position&, const Context&)>;

  /// Recreate supplies the same capability on a reconstructed owner, not an
  /// independently allocated subtree. Like other navigation factories, it must
  /// capture session services, never the temporary owner or borrowed Context.
  explicit VirtualSubtree(Factory recreate, Targets children = {}, Targets crosses = {},
                          Visitable visitable = {})
      : recreate_(std::move(recreate)), children_(std::move(children)),
        crosses_(std::move(crosses)), visitable_(std::move(visitable)) {}

  /// Publish this member through an aliasing shared_ptr. Ownership stays with UI;
  /// Manager's strong transition references therefore also keep the UI alive.
  /// Call after constructing the owner with shared ownership. The weak alias
  /// retained internally cannot create an owner/member reference cycle.
  template <class Owner>
  static std::shared_ptr<INavigable> retained(const std::shared_ptr<Owner>& owner,
                                              VirtualSubtree Owner::* member) {
    if (!owner) throw std::logic_error("Virtual subtree requires a retained owner");
    auto& subtree = owner.get()->*member;
    std::shared_ptr<INavigable> alias(owner, &subtree);
    subtree.self_ = alias;
    return alias;
  }

  /// Stable position identity plus reconstruction, suitable for durable Paths.
  Anchor anchor(Position value) const { return Anchor{std::move(value)}.with_factory(recreate_); }
  /// Interpret this path level, never a shared mutable current-node field.
  const Position& position(const Context& context) const {
    if (context.depth == 0 || context.depth > context.path.size())
      throw std::logic_error("Virtual subtree position is outside Context");
    const auto* value = context.path[context.depth - 1].template get_if<Position>();
    if (!value) throw std::logic_error("Virtual subtree received another position type");
    return *value;
  }
  /// Direct structural action: the owner already knows the requested child, so
  /// issuance needs no enumeration. The owner defines which actions are legal.
  Target child(Position value, const Context& context) const {
    return target(anchor(std::move(value)), context);
  }
  /// Source-owned Cross declaration; mode travels with the issued action.
  Target cross(Location destination, CrossMode mode, const Context& context) const {
    return target(std::move(destination), mode, context);
  }
  bool represents(const Anchor& value, const Context&) const override {
    return value.template get_if<Position>() != nullptr;
  }
  bool visitable(const Context& context) const override {
    const auto& value = position(context);
    return !visitable_ || visitable_(value, context);
  }
  std::vector<Target> children(const Context& context) const override {
    return children_ ? children_(*this, context) : std::vector<Target>{};
  }
  std::vector<Target> cross_targets(const Context& context) const override {
    return crosses_ ? crosses_(*this, context) : std::vector<Target>{};
  }
  /// Every position uses this same capability. The returned alias pins the owner,
  /// even if its presentation host releases it during the transition.
  std::shared_ptr<INavigable> resolve_child(const Anchor& value,
                                            const Context& context) const override {
    return represents(value, context) ? self_.lock() : nullptr;
  }

private:
  Factory recreate_;
  Targets children_;
  Targets crosses_;
  Visitable visitable_;
  std::weak_ptr<INavigable> self_;
};
} // namespace folio::tui::navigation
