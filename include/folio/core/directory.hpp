#pragma once

#include "folio/core/types.hpp"

#include <string>

namespace folio::core {

class Directory {
 public:
  explicit Directory(std::string name);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept;

  void set_name(std::string name);

 private:
  const Id id_;
  std::string name_;  // UTF-8.
};

}  // namespace folio::core
