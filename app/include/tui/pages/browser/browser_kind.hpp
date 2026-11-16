#pragma once
#include "tui/logic/navigation/navigation.hpp"

#include <folio/core/types.hpp>
#include <cstddef>
#include <optional>

namespace folio::tui {
/// Browser presentation/query context; static navigation uses the UI class type.
enum class BrowserKind { Logs, Memos, Todos };
/// Translate the existing tab/query context to concrete UI navigation identity.
navigation::Anchor browser_anchor(BrowserKind browser);
std::optional<BrowserKind> browser_kind(const navigation::Anchor& anchor);

/// Provisional semantic entry cursor snapshot; never an FTXUI object address.
/// TODO(Navigation): Replace with virtual-subtree Location restoration, not a
/// parallel navigation-position model. See Manager::remember_position().
struct BrowserPosition {
  std::size_t index = 0;
  std::optional<core::Id> entry;
};
} // namespace folio::tui
