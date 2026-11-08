#pragma once

#include "io/internal/atomic_write.hpp"

namespace folio::io::internal {
// Testing only: inject failures in lifecycle durability helpers, not normal
// Repository writes. Thread-local; tests must restore the returned prior hook.
SyncFile set_lifecycle_sync_for_testing(SyncFile sync) noexcept;
} // namespace folio::io::internal
