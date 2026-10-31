#pragma once

#include "storage/filesystem.hpp"

#include <optional>
#include <utility>

namespace folio::application::internal::testing {

// Test-only, thread-local injection. Production leaves this unset and resolves
// configuration normally. The scope restores any prior override automatically.
inline thread_local std::optional<core::storage::StoragePaths> configuration_override;

class ScopedConfigurationOverride {
 public:
  explicit ScopedConfigurationOverride(core::storage::StoragePaths paths)
      : previous_(std::exchange(configuration_override, std::move(paths))) {}

  ~ScopedConfigurationOverride() {
    configuration_override = std::move(previous_);
  }

  ScopedConfigurationOverride(const ScopedConfigurationOverride&) = delete;
  ScopedConfigurationOverride& operator=(const ScopedConfigurationOverride&) = delete;

 private:
  std::optional<core::storage::StoragePaths> previous_;
};

}  // namespace folio::application::internal::testing
