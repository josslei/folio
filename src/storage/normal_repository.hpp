#pragma once

#include "folio/core/directory.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"
#include "folio/core/todo.hpp"
#include "storage/filesystem.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace folio::core::storage {

// Synchronous plaintext persistence. The root is supplied by the caller.
// Entry locators returned here are relative to normal_root. The caller must
// exclude external filesystem mutation during an operation, as required by
// the single-file atomic writer. Stored UUIDs are authoritative for entries;
// existing entry filenames may differ from generated canonical names, while
// Logs/Memos require .md and Todos require .toml. Generated atomic temporary
// names lack those extensions, and .folio-dir.toml is Directory metadata.
class NormalRepository {
 public:
  explicit NormalRepository(std::filesystem::path normal_root);

  [[nodiscard]] Result<std::monostate> create_log(const core::Log::Snapshot& snapshot);
  [[nodiscard]] Result<std::monostate> create_memo(const core::Memo::Snapshot& snapshot);
  [[nodiscard]] Result<std::monostate> create_todo(const core::Todo::Snapshot& snapshot);

  // A supplied locator is read directly, without discovery or fallback.
  // It must still contain the requested stored UUID.
  [[nodiscard]] Result<core::Log::Snapshot> load_log(
      const core::Id& id,
      std::optional<std::filesystem::path> relative_path = std::nullopt) const;
  [[nodiscard]] Result<core::Memo::Snapshot> load_memo(
      const core::Id& id,
      std::optional<std::filesystem::path> relative_path = std::nullopt) const;
  [[nodiscard]] Result<core::Todo::Snapshot> load_todo(
      const core::Id& id,
      std::optional<std::filesystem::path> relative_path = std::nullopt) const;

  [[nodiscard]] Result<std::monostate> update_log(
      const core::Log::Snapshot& snapshot,
      std::optional<std::filesystem::path> relative_path = std::nullopt);
  [[nodiscard]] Result<std::monostate> update_memo(
      const core::Memo::Snapshot& snapshot,
      std::optional<std::filesystem::path> relative_path = std::nullopt);
  [[nodiscard]] Result<std::monostate> update_todo(
      const core::Todo::Snapshot& snapshot,
      std::optional<std::filesystem::path> relative_path = std::nullopt);

  [[nodiscard]] Result<std::monostate> create_directory(
      DirectoryKind kind, const core::Directory::Snapshot& snapshot);
  [[nodiscard]] Result<core::Directory::Snapshot> load_directory(
      DirectoryKind kind, const core::Id& id) const;
  [[nodiscard]] Result<std::monostate> rename_directory(
      DirectoryKind kind, const core::Id& id, std::string name);

  [[nodiscard]] Result<std::monostate> move_memo(
      const core::Id& id, std::optional<core::Id> directory_id,
      core::TimestampContext modified);
  [[nodiscard]] Result<std::monostate> move_todo(
      const core::Id& id, std::optional<core::Id> directory_id,
      core::TimestampContext modified);

  [[nodiscard]] Result<std::filesystem::path> locate_entry(
      EntryType type, const core::Id& id) const;
  [[nodiscard]] Result<std::filesystem::path> locate_entry(
      EntryType type, const std::filesystem::path& relative_path) const;

 private:
  std::filesystem::path root_;
};

}  // namespace folio::core::storage
