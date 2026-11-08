#pragma once

#include "folio/core/types.hpp"

#include <string>

namespace folio::core {

enum class DirectoryKind { Memo, Todo };

class Directory {
public:
  /// Complete owning domain value for persistence and transfer, not an editing
  /// permission or concurrency token. Restoring does not generate a new identity.
  struct Snapshot {
    Id id;
    DirectoryKind kind;
    std::string name;
  };

  Directory(DirectoryKind kind, std::string name);

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] static Directory restore(const Snapshot& snapshot);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] DirectoryKind kind() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept;

  void set_name(std::string name);

private:
  explicit Directory(const Snapshot& snapshot);

  const Id id_;
  const DirectoryKind kind_;
  std::string name_; // UTF-8.
};

} // namespace folio::core
