#pragma once

#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>

namespace folio::core::storage {
class MetadataCatalog;
class NormalRepository;
class CatalogCoordinator;
}

namespace folio::application::internal {

struct BrowseRequest {
  DirectoryRef dir;
  RecordOrder order;
};

struct QueryRecordsRequest { RecordQuery query; };
struct ListDirectoriesRequest { core::DirectoryKind kind; };
struct LoadLogRequest { core::Id id; };
struct LoadMemoRequest { core::Id id; };
struct LoadTodoRequest { core::Id id; };

using AccessRequest = std::variant<
    BrowseRequest, QueryRecordsRequest,
    ListDirectoriesRequest, LoadLogRequest, LoadMemoRequest, LoadTodoRequest>;

using AccessResponse = std::variant<
    ApplicationResponse<BrowseResult>,
    ApplicationResponse<std::vector<core::RecordMetadata>>,
    ApplicationResponse<std::vector<core::DirectoryMetadata>>,
    core::Log::Snapshot, core::Memo::Snapshot, core::Todo::Snapshot,
    ApplicationError>;

// Called only by the Backend's single execution owner. Metadata stays in memory;
// content access uses current known locators without implicit discovery.
AccessResponse handle_access(const AccessRequest& request,
                             core::storage::MetadataCatalog& catalog,
                             core::storage::NormalRepository& repository,
                             core::storage::CatalogCoordinator& coordinator);

}  // namespace folio::application::internal
