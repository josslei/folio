#pragma once

#include "storage/common.hpp"

namespace folio::core::storage::internal {

// Test-only failure-injection hooks, not an API for normal Repository callers.
// Declared beside the implementation because Repository code invokes these seams.
// Null hooks use the production implementations. Tests must restore hooks.
struct RepositoryTestHooks {
  StorageResult<std::monostate> (*write)(const std::filesystem::path&, std::string_view,
                                         bool create_only) = nullptr;
  bool (*remove_source)(const std::filesystem::path&, std::error_code&) = nullptr;
  bool (*sync_source)(int) = nullptr;
  void (*before_projection)() = nullptr;
};
extern thread_local RepositoryTestHooks repository_test_hooks;

} // namespace folio::core::storage::internal
