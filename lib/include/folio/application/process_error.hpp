#pragma once

#include <string>
#include <system_error>

namespace folio::application {

enum class ProcessErrorCode { InvalidArgument, IoError, AlreadyOwned, LaunchFailed };

struct ProcessError {
  ProcessErrorCode code;
  std::string message;
  std::error_code system_error;
};

} // namespace folio::application
