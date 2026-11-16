#pragma once

#include <any>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace folio::tui::navigation {
class INavigable;
struct Description;
/// Class-owned static relationship metadata, available without constructing UI.
template <class UI> const Description& description();
/// Construction callable retained by declarations/paths. Captures must not own
/// UI instances or borrow temporary navigation context.
using Factory = std::function<std::shared_ptr<INavigable>()>;
/// Stable, owner-interpreted reference to one step in a navigation path, not a
/// live tree node or a complete Location. Stores copyable semantic identity plus
/// optional registration/construction capabilities; never retains a UI object.
/// Equality compares identity only, not factories or registration accessors.
class Anchor {
public:
  /// Static UI identity, independent of any instance and valid for incomplete UI types.
  template <class UI> static Anchor of();
  /// Class identity plus direct access to process-lifetime registration metadata.
  template <class UI> static Anchor registered();
  /// Relationship metadata when attached; null does not mean an invalid anchor.
  const Description* registration() const;
  /// Construct an instance without mounting/selecting it; throws if no factory.
  std::shared_ptr<INavigable> construct() const;
  /// Copy this identity with a session-bound construction capability.
  Anchor with_factory(Factory factory) const {
    auto result = *this;
    result.factory_ = std::move(factory);
    return result;
  }
  template <class UI> bool is() const;

  template <class T>
    requires(!std::same_as<T, Anchor>) && std::copy_constructible<T> && std::equality_comparable<T>
  explicit Anchor(T value);

  /// Read the owner's typed identity; null if this anchor stores another type.
  template <class T> const T* get_if() const;
  bool operator==(const Anchor& other) const;

private:
  friend class INavigable;
  template <class UI> struct Type {
    bool operator==(const Type&) const;
  };

  std::any value_;
  bool (*equals_)(const std::any&, const std::any&);
  std::function<const Description&()> registration_;
  Factory factory_;
};

/// Registration metadata, not a live INavigable or a second hierarchy tree.
/// Class-owned static definitions describe structure; instance
/// copies may bind session factories. Child getters never construct UI. Dynamic
/// children remain authoritative through INavigable, not this static list.
struct Description {
  Anchor anchor;
  bool visitable = true;
  std::vector<std::function<const Description&()>> children;
  /// Session-bound construction; captures services, never a temporary UI instance.
  Factory create;
};

// TODO(Navigation): If Location Paths become substantially deeper or path copying
// becomes a measured performance cost, evaluate persistent/shared-prefix Location
// Paths (structural sharing) instead of copying full vectors.
/// Complete restorable structural path, starting at a Page below the implicit
/// Shell root. Each owner interprets its child's anchor. Contains no UI pointers;
/// Current Location and History can outlive temporary components.
using Location = std::vector<Anchor>;

/// Transition-local context; nothing here may be retained by a UI object.
/// path is the destination (or current path during discovery); depth is this
/// owner's prefix length, with depth <= path.size().
struct Context {
  const Location& path;
  std::size_t depth = 0;
};

/// History policy declared with a Cross action, not chosen again by its caller.
enum class CrossMode {
  Push,  ///< Preserve the actual source as a return step; clear Forward.
  Switch ///< No additional return step; clear Forward.
};
/// Outcome of a navigation operation; operational Backend errors are separate.
enum class Result { Changed, Unchanged };

/// A transition issued by its declaring owner, not an arbitrary destination.
/// Contains semantic values and factory metadata only; never retains a UI object.
/// A Cross declaration includes its mode; separate actions may share a destination.
/// Unlike Anchor (one path step) or Location (a path), Target is permission from
/// an owner to request a transition. No mode means a structural Down action.
class Target {
public:
  const Location& source() const;
  const Location& destination() const;
  std::optional<CrossMode> mode() const;

private:
  friend class INavigable;
  Target(Location source, Location destination, std::optional<CrossMode> mode);
  Location source_;
  Location destination_;
  std::optional<CrossMode> mode_;
};

/// Common contract implemented by live navigation-capable UI objects/adapters.
/// Owners expose their own relationships; Manager does not maintain a registry.
/// TUI-thread navigation capabilities. Resolution only reads existing ownership;
/// construction belongs to Description::create, and host mounts an independently
/// prepared destination without selecting it. Manager commits selection afterward.
class INavigable {
public:
  INavigable();
  INavigable(const INavigable&) = delete;
  INavigable& operator=(const INavigable&) = delete;
  virtual ~INavigable();
  /// Optional static registration metadata; dynamic implementations may omit it.
  virtual const Description* static_description() const;
  /// Whether this implementation represents the supplied semantic path step.
  /// Virtual capabilities interpret typed values rather than static class identity.
  virtual bool represents(const Anchor& anchor, const Context& context) const;
  /// Whether this capability describes a location that can itself be visited.
  virtual bool visitable(const Context&) const;
  /// Children relative to the supplied logical position, static or dynamic.
  virtual std::vector<Target> children(const Context& context) const = 0;
  /// Declared non-structural jumps, each carrying its Push/Switch policy.
  virtual std::vector<Target> cross_targets(const Context& context) const = 0;
  /// Resolve one known child without construction, mounting, or selection changes.
  /// A retained existing instance, or empty when construction is necessary.
  virtual std::shared_ptr<INavigable> resolve_child(const Anchor& child,
                                                    const Context& context) const = 0;
  /// Mount a factory-created destination, composing necessary visible ancestors.
  /// Normal return guarantees the full destination path resolves to the retained
  /// exact destination. Exceptions must leave current presentation intact.
  /// No synchronous IPC or navigation re-entry; selection follows Manager's commit.
  virtual void host(const std::shared_ptr<INavigable>&, const Context&);

protected:
  /// Dynamic relationship declaration. Static declarations use the same issuance
  /// path. The owner supplies its actual relationships; Manager trusts the target.
  Target target(const Anchor& child, const Context& context) const;
  Target target(Anchor child, Factory factory, const Context& context) const;
  Target target(Location destination, CrossMode mode, const Context& context) const;
};

// Inline member definitions are intentionally grouped after all participating
// types. Anchor::construct reads Description::create, while Description owns a
// Anchor by value. Defining construct in the class would therefore create a
// declaration-order cycle; keeping the member implementations here makes that
// ordering explicit without introducing another source file.

template <class UI> inline bool Anchor::Type<UI>::operator==(const Type&) const = default;

template <class UI> inline Anchor Anchor::of() {
  return Anchor{Type<UI>{}};
}

template <class UI> inline Anchor Anchor::registered() {
  auto result = of<UI>();
  result.registration_ = &description<UI>;
  return result;
}

inline const Description* Anchor::registration() const {
  return registration_ ? &registration_() : nullptr;
}

inline std::shared_ptr<INavigable> Anchor::construct() const {
  if (factory_) return factory_();
  const auto* metadata = registration();
  if (!metadata || !metadata->create)
    throw std::logic_error("Navigation destination has no construction capability");
  return metadata->create();
}

template <class UI> inline bool Anchor::is() const {
  return get_if<Type<UI>>() != nullptr;
}

template <class T>
  requires(!std::same_as<T, Anchor>) && std::copy_constructible<T> && std::equality_comparable<T>
inline Anchor::Anchor(T value)
    : value_(std::move(value)), equals_([](const std::any& a, const std::any& b) {
        const auto* other = std::any_cast<T>(&b);
        return other && std::any_cast<const T&>(a) == *other;
      }) {}

template <class T> inline const T* Anchor::get_if() const {
  return std::any_cast<T>(&value_);
}

inline bool Anchor::operator==(const Anchor& other) const {
  return equals_(value_, other.value_);
}

inline Target::Target(Location source, Location destination, std::optional<CrossMode> mode)
    : source_(std::move(source)), destination_(std::move(destination)), mode_(mode) {}

inline const Location& Target::source() const {
  return source_;
}

inline const Location& Target::destination() const {
  return destination_;
}

inline std::optional<CrossMode> Target::mode() const {
  return mode_;
}

inline INavigable::INavigable() = default;

inline INavigable::~INavigable() = default;

inline const Description* INavigable::static_description() const {
  return nullptr;
}

inline bool INavigable::represents(const Anchor& anchor, const Context&) const {
  const auto* metadata = static_description();
  return !metadata || metadata->anchor == anchor;
}

inline bool INavigable::visitable(const Context&) const {
  return true;
}

inline void INavigable::host(const std::shared_ptr<INavigable>&, const Context&) {
  throw std::logic_error("Navigation owner cannot host this destination");
}

inline Target INavigable::target(const Anchor& child, const Context& context) const {
  if (context.depth > context.path.size())
    throw std::logic_error("Navigation owner context exceeds its path");
  Location source(context.path.begin(), context.path.begin() + context.depth);
  auto destination = source;
  destination.push_back(child);
  return Target{std::move(source), std::move(destination), {}};
}

inline Target INavigable::target(Anchor child, Factory factory, const Context& context) const {
  child.factory_ = std::move(factory);
  return target(child, context);
}

inline Target INavigable::target(Location destination, CrossMode mode,
                                 const Context& context) const {
  return Target{context.path, std::move(destination), mode};
}
} // namespace folio::tui::navigation
