#include "folio/core/types.hpp"

#include <sodium.h>

#include <stdexcept>

namespace folio::core {

void ensure_sodium_initialized() {
  static const bool initialized = [] {
    if (sodium_init() < 0) {
      throw std::runtime_error("Failed to initialize libsodium");
    }
    return true;
  }();

  (void)initialized;
}

}  // namespace folio::core
