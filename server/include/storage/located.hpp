#pragma once

#include <filesystem>

namespace folio::core::storage {

// Associates shared metadata with private persistence location information.
// The current Normal locator is relative to normal_root. Future Vault locator
// requirements remain private here; no Vault representation is chosen yet.
template<class Metadata>
struct Located {
  Metadata metadata;
  std::filesystem::path relative_path;
};

}  // namespace folio::core::storage
