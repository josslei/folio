#include "folio/core/types.hpp"

#include <sodium.h>

#include <stdexcept>
#include <utility>

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

Collection::Collection(std::string name)
    : id_(Id::generate()), name_(std::move(name)) {}

const Id& Collection::id() const noexcept { return id_; }
const std::string& Collection::name() const noexcept { return name_; }

void Collection::set_name(std::string name) { name_ = std::move(name); }

}  // namespace folio::core
