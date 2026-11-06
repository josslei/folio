#pragma once
#include "io/internal/filesystem_test_hooks.hpp"
#include "io/internal/atomic_write.hpp"
namespace folio::core::storage::internal {
using io::internal::set_lifecycle_sync_for_testing;
}
