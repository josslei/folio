#pragma once

#include <folio/application/query.hpp>

namespace folio::application {

/// The entire Normal Repository, without exposing its physical layout.
struct RepositoryScope {};
enum class RefreshTraversal { Immediate, Recursive };
/// Absence removes metadata only within the selected scan scope. A user Directory
/// has no nested Directories; either traversal visits its immediate Records.
struct DirectoryScanScope {
  DirectoryRef directory;
  RefreshTraversal traversal = RefreshTraversal::Recursive;
};
/// Whole-Repository refresh has no traversal option; it always rebuilds everything.
using RefreshScope = std::variant<RepositoryScope, DirectoryScanScope>;

/// A heuristic observation, never certification of complete consistency.
enum class ConsistencyCheckResult { NoAnomalyDetected, AnomalyDetected };

} // namespace folio::application
