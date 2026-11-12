#pragma once

#include <folio/core/result.hpp>

#include <string>
#include <vector>

namespace folio::application {

enum class ErrorCategory {
  InvalidRequest,
  TargetUnavailable,
  FilesystemFailure,
  AuthoritativeDataProblem,
  ConnectionFailure,
  InternalFailure,
  ApplicationUnavailable
};

enum class ErrorReason {
  InvalidQuery,
  WrongRecordType,
  NotFound,
  StaleLocation,
  InvalidData,
  UnsupportedVersion,
  IdentityConflict,
  UnsafePath,
  AccessDenied,
  IoFailure,
  ConnectionLost,
  Timeout,
  BackendUnavailable,
  ProtocolError,
  IncompatibleProtocol,
  MetadataUnavailable,
  ResourceLimit,
  UnexpectedFailure,
  NotImplemented,
  InvalidMutation,
  LockConflict,
  EditLockRequired,
  CatalogSynchronizationFailed,
  FullRefreshRequired,
  EntryWriteProtected
};

// Presentation choices, not automatic retries or guarantees of success.
// Authoritative Recovery remains deferred; Refresh rebuilds Catalog only.
// See docs/contracts/mutations.md.
enum class ErrorAction { Retry, ReloadView, Reconnect, ResolveExternally, Dismiss, Refresh };

/// Safe presentation data: why the operation failed and possible user actions.
/// Actions are suggestions, not commands. For mutations, consult status/evidence
/// separately; this error alone does not establish whether data was persisted.
struct ApplicationError {
  ErrorCategory category;
  ErrorReason reason;
  std::string message;
  std::vector<ErrorAction> available_actions;
};

enum class WarningReason { CatalogInconsistent, EntryReadOnly };

struct ApplicationWarning {
  WarningReason reason;
  std::string message;
  std::vector<ErrorAction> available_actions;
};

/// Successful Access data accompanied by Application warnings.
template <class T> using Annotated = core::Annotated<T, std::vector<ApplicationWarning>>;

template <class T> using AccessResult = core::Result<T, ApplicationError>;

} // namespace folio::application
