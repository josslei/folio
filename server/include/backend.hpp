#pragma once

namespace folio::application {

// Resolves configuration, acquires ownership, and runs the Backend lifecycle.
// Exit code 4 indicates that another Backend already owns the Folio Repo.
int run_backend();

}  // namespace folio::application
