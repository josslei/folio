#pragma once

#include <filesystem>

namespace folio::core::storage {

// Associates shared metadata with private persistence location information.
// The current Normal locator is relative to normal_root. Future Vault locator
// requirements remain private here; no Vault representation is chosen yet.
template <class Metadata> struct Located {
  Metadata metadata;
  std::filesystem::path relative_path;
  /// Backend-owned scan assessment, persisted in Index and published with metadata.
  /// Valid under the no-runtime-external-modification contract; never supplied
  /// or overridden by a Frontend Mutation request.
  bool read_only = false;
};

} // namespace folio::core::storage
