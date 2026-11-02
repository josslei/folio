#pragma once

#include <folio/core/result.hpp>

#include <string>
#include <vector>

namespace folio::application {

enum class ErrorCategory {
  InvalidRequest, TargetUnavailable, FilesystemFailure, AuthoritativeDataProblem,
  ConnectionFailure, InternalFailure, ApplicationUnavailable
};

enum class ErrorReason {
  InvalidQuery, WrongRecordType, NotFound, StaleLocation, InvalidData,
  UnsupportedVersion, IdentityConflict, UnsafePath, AccessDenied, IoFailure,
  ConnectionLost, Timeout, BackendUnavailable, ProtocolError, IncompatibleProtocol,
  MetadataUnavailable, ResourceLimit, UnexpectedFailure, NotImplemented
};

// Presentation choices, not automatic retries or guarantees of success.
// TODO(4.2b/Recovery): operation outcomes and safe mutation reconciliation.
enum class ErrorAction { Retry, ReloadView, Reconnect, ResolveExternally, Dismiss };

struct ApplicationError {
  ErrorCategory category;
  ErrorReason reason;
  std::string message;
  std::vector<ErrorAction> available_actions;
};

enum class WarningReason { CatalogInconsistent };

struct ApplicationWarning {
  WarningReason reason;
  std::string message;
  std::vector<ErrorAction> available_actions;
};

template<class T>
struct ApplicationResponse {
  T value;
  std::vector<ApplicationWarning> warnings;
};

template<class T>
using ApplicationResult = core::Result<T, ApplicationError>;

}  // namespace folio::application
