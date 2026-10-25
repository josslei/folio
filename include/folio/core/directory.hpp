#pragma once

#include "folio/core/types.hpp"

#include <string>

namespace folio::core {

class Directory {
 public:
  struct Snapshot {
    Id id;
    std::string name;
  };

  explicit Directory(std::string name);

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] static Directory restore(const Snapshot& snapshot);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept;

  void set_name(std::string name);

 private:
  explicit Directory(const Snapshot& snapshot);

  const Id id_;
  std::string name_;  // UTF-8.
};

}  // namespace folio::core
