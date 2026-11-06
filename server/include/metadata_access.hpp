#pragma once

#include <folio/ipc/application_protocol.hpp>

namespace folio::core::storage {
class MetadataCatalog;
class NormalRepository;
class CatalogCoordinator;
}

namespace folio::application::internal {

// Called only by the Backend's single execution owner. Metadata stays in memory;
// content access uses current known locators without implicit discovery.
AccessResponse handle_access(const AccessRequest& request,
                             core::storage::MetadataCatalog& catalog,
                             core::storage::NormalRepository& repository,
                             core::storage::CatalogCoordinator& coordinator);

}  // namespace folio::application::internal
