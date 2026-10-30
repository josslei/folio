#pragma once

#include "folio/core/types.hpp"

namespace folio::core::detail {

[[nodiscard]] constexpr bool is_valid_privacy(Privacy privacy) noexcept {
  switch (privacy) {
    case Privacy::Normal:
    case Privacy::Protected:
    case Privacy::Hidden:
      return true;
  }
  return false;
}

}  // namespace folio::core::detail
