#pragma once

#include <memory>
#include <functional>

namespace folio::application::internal {
// Lifetime identity, never a claimed PID or filesystem locator. A worker and
// each of its accepted executor actions retain this token. Locks borrow it
// weakly, so disconnect cannot revoke ownership before accepted work completes.
struct ConnectionSession {
  /// Installed by editing control; runs only after all accepted actions release ownership.
  /// Cleanup must not throw or block on I/O; notification sinks contain delivery errors.
  std::function<void()> released;
  ~ConnectionSession() {
    if (released) released();
  }
};
using Session = std::shared_ptr<ConnectionSession>;
} // namespace folio::application::internal
