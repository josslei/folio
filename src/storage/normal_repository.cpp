#include "storage/normal_repository.hpp"

#include "storage/codec.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace folio::core::storage {
namespace {

namespace fs = std::filesystem;
namespace codec = ::folio::core::storage::codec;

StorageError fail(StorageErrorCode code, const fs::path& path, std::string message,
                  std::error_code ec = {}, bool published = false) {
  return {code, path, std::move(message), ec, published};
}

StorageError codec_failure(const fs::path& path, const codec::CodecError& error) {
  return fail(StorageErrorCode::CodecError, path,
              "Invalid serialized entry at " + error.path + ": " + error.message);
}

template <class T>
const StorageError* failure(const Result<T>& result) {
  return std::get_if<StorageError>(&result);
}

fs::path module_name(EntryType type) {
  switch (type) {
    case EntryType::Log: return "logs";
    case EntryType::Memo: return "memos";
    case EntryType::Todo: return "todos";
  }
  return {};
}

EntryType entry_type(DirectoryKind kind) {
  return kind == DirectoryKind::Memo ? EntryType::Memo : EntryType::Todo;
}

bool valid_kind(DirectoryKind kind) {
  return kind == DirectoryKind::Memo || kind == DirectoryKind::Todo;
}

bool valid_type(EntryType type) {
  return type == EntryType::Log || type == EntryType::Memo || type == EntryType::Todo;
}

bool under(const fs::path& root, const fs::path& child) {
  auto a = root.begin();
  auto b = child.begin();
  for (; a != root.end(); ++a, ++b) {
    if (b == child.end() || *a != *b) return false;
  }
  return true;
}

bool has_nul(const fs::path& path) {
  return path.native().find(fs::path::value_type{}) != fs::path::string_type::npos;
}

// Check each existing component from the configured root downward. An
// existing alias in the system's temporary-directory ancestry is outside
// the repository boundary; a symlink at or below the root is rejected.
Result<std::monostate> safe_path(const fs::path& root, const fs::path& path,
                                bool allow_missing = false) {
  if (!root.is_absolute() || has_nul(root) || path.empty() || has_nul(path) ||
      !path.is_absolute() ||
      !under(root.lexically_normal(), path.lexically_normal()) ||
      path != path.lexically_normal()) {
    return fail(StorageErrorCode::InvalidPath, path, "Path escapes the Normal repository");
  }
  fs::path current = root;
  const auto relative = path.lexically_relative(root);
  auto check = [&](const fs::path& component) -> Result<std::monostate> {
    const auto current_path = component;
    std::error_code ec;
    const auto status = fs::symlink_status(current_path, ec);
    if (ec == std::errc::no_such_file_or_directory) {
      if (allow_missing) return std::monostate{};
      return fail(StorageErrorCode::IoError, current_path, "Path does not exist", ec);
    }
    if (ec) return fail(StorageErrorCode::IoError, current_path, "Could not inspect path", ec);
    if (fs::is_symlink(status)) {
      return fail(StorageErrorCode::InvalidPath, current_path, "Symlink in repository path");
    }
    return std::monostate{};
  };
  if (auto checked = check(current); failure(checked)) return *failure(checked);
  for (const auto& part : relative) {
    current /= part;
    if (auto checked = check(current); failure(checked)) return *failure(checked);
  }
  return std::monostate{};
}

Result<std::monostate> ensure_directory(const fs::path& root, const fs::path& path) {
  if (auto checked = safe_path(root, path, true); failure(checked)) return *failure(checked);
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) return fail(StorageErrorCode::IoError, path, "Could not create directory", ec);
  if (auto checked = safe_path(root, path); failure(checked)) return *failure(checked);
  if (!fs::is_directory(path, ec) || ec) {
    return fail(StorageErrorCode::InvalidPath, path, "Expected a directory", ec);
  }
  return std::monostate{};
}

Result<std::string> read_text(const fs::path& root, const fs::path& path) {
  if (auto checked = safe_path(root, path); failure(checked)) return *failure(checked);
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    return fail(StorageErrorCode::InvalidPath, path, "Expected a regular file", ec);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return fail(StorageErrorCode::IoError, path, "Could not open file");
  std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  if (input.bad()) return fail(StorageErrorCode::IoError, path, "Could not read file");
  return text;
}

template <class Snapshot>
Result<std::string> encode(const Snapshot& snapshot, const fs::path& path) {
  try {
    if constexpr (std::is_same_v<Snapshot, core::Log::Snapshot> ||
                  std::is_same_v<Snapshot, core::Memo::Snapshot>) {
      auto encoded = codec::serialize_markdown(snapshot);
      if (auto* problem = std::get_if<codec::CodecError>(&encoded)) {
        return codec_failure(path, *problem);
      }
      return std::get<std::string>(std::move(encoded));
    } else {
      auto encoded = codec::serialize_toml(codec::to_toml(snapshot));
      if (auto* problem = std::get_if<codec::CodecError>(&encoded)) {
        return codec_failure(path, *problem);
      }
      return std::get<std::string>(std::move(encoded));
    }
  } catch (const std::exception& exception) {
    return fail(StorageErrorCode::CodecError, path,
                std::string{"Could not encode entry: "} + exception.what());
  }
}

template <class Snapshot>
Result<Snapshot> decode(std::string_view text, const fs::path& path,
                        std::optional<DirectoryKind> trusted_legacy_kind = std::nullopt) {
  if constexpr (std::is_same_v<Snapshot, core::Log::Snapshot>) {
    auto decoded = codec::log_snapshot_from_markdown(text);
    if (auto* problem = std::get_if<codec::CodecError>(&decoded)) {
      return codec_failure(path, *problem);
    }
    return std::get<Snapshot>(std::move(decoded));
  } else if constexpr (std::is_same_v<Snapshot, core::Memo::Snapshot>) {
    auto decoded = codec::memo_snapshot_from_markdown(text);
    if (auto* problem = std::get_if<codec::CodecError>(&decoded)) {
      return codec_failure(path, *problem);
    }
    return std::get<Snapshot>(std::move(decoded));
  } else {
    auto parsed = codec::parse_toml(text);
    if (auto* problem = std::get_if<codec::CodecError>(&parsed)) {
      return codec_failure(path, *problem);
    }
    if constexpr (std::is_same_v<Snapshot, core::Todo::Snapshot>) {
      auto decoded = codec::todo_snapshot_from_toml(std::get<toml::table>(parsed));
      if (auto* problem = std::get_if<codec::CodecError>(&decoded)) {
        return codec_failure(path, *problem);
      }
      return std::get<Snapshot>(std::move(decoded));
    } else {
      auto decoded = codec::directory_snapshot_from_toml(
          std::get<toml::table>(parsed), trusted_legacy_kind);
      if (auto* problem = std::get_if<codec::CodecError>(&decoded)) {
        return codec_failure(path, *problem);
      }
      return std::get<Snapshot>(std::move(decoded));
    }
  }
}

template <class Snapshot>
Result<std::string> checked_encoding(const Snapshot& snapshot, const fs::path& path) {
  if constexpr (!std::is_same_v<Snapshot, core::Directory::Snapshot>) {
    if (snapshot.privacy != core::Privacy::Normal) {
      return fail(StorageErrorCode::InvalidPath, path,
                  "Normal repository accepts only Normal privacy");
    }
  }
  auto encoded = encode(snapshot, path);
  if (failure(encoded)) return *failure(encoded);
  auto decoded = decode<Snapshot>(std::get<std::string>(encoded), path);
  if (failure(decoded)) return *failure(decoded);
  return encoded;
}

Result<std::string> filename(EntryType type, const core::Id& id,
                             const core::TimestampContext& created) {
  return normal_entry_filename(type, id, created.timestamp);
}

struct Match {
  EntryType type;
  fs::path path;
};

template <class Snapshot>
Result<Snapshot> load_at(const fs::path& root, EntryType type, const fs::path& path);

Result<core::Directory::Snapshot> load_directory_at(
    const fs::path& root, DirectoryKind kind, const core::Id& id);

Result<core::Id> checked_entry_id(const fs::path& root, EntryType type,
                                   const fs::path& path);

Result<std::vector<Match>> scan_for_id(const fs::path& root, const core::Id& id) {
  std::vector<Match> matches;
  std::set<std::string> seen_ids;
  for (auto type : {EntryType::Log, EntryType::Memo, EntryType::Todo}) {
    const auto base = root / module_name(type);
    if (auto checked = safe_path(root, base, true); failure(checked)) return *failure(checked);
    std::error_code ec;
    if (!fs::exists(base, ec)) {
      if (ec) return fail(StorageErrorCode::IoError, base, "Could not inspect module", ec);
      continue;
    }
    if (!fs::is_directory(base, ec) || ec) {
      return fail(StorageErrorCode::InvalidPath, base, "Module path is not a directory", ec);
    }
    auto visit = [&](const fs::path& folder) -> Result<std::monostate> {
      fs::directory_iterator item{folder, ec};
      const fs::directory_iterator end;
      while (!ec && item != end) {
        const auto candidate = item->path();
        if (auto checked = safe_path(root, candidate); failure(checked)) return *failure(checked);
        const auto status = item->symlink_status(ec);
        if (ec) break;
        const auto extension = type == EntryType::Todo ? ".toml" : ".md";
        if (fs::is_regular_file(status) && candidate.extension() == extension &&
            candidate.filename() != ".folio-dir.toml") {
          auto stored_id = checked_entry_id(root, type, candidate);
          if (failure(stored_id)) return *failure(stored_id);
          const auto& value = std::get<core::Id>(stored_id);
          if (!seen_ids.insert(value.to_string()).second) {
            return fail(StorageErrorCode::DuplicateId, candidate,
                        "Stored entry UUID occurs in more than one formal file");
          }
          if (value == id) {
            matches.push_back({type, candidate});
          }
        } else if (type == EntryType::Log && fs::is_directory(status)) {
          return fail(StorageErrorCode::RecoveryRequired, candidate,
                      "Unexpected folder in flat Log module");
        }
        item.increment(ec);
      }
      if (ec) return fail(StorageErrorCode::IoError, folder, "Could not scan module", ec);
      return std::monostate{};
    };
    if (auto checked = visit(base); failure(checked)) return *failure(checked);
    if (type == EntryType::Log) continue;
    fs::directory_iterator item{base, ec};
    const fs::directory_iterator end;
    while (!ec && item != end) {
      const auto folder = item->path();
      if (auto checked = safe_path(root, folder); failure(checked)) return *failure(checked);
      if (item->is_directory(ec)) {
        if (ec) break;
        const auto folder_id = core::Id::parse(folder.filename().string());
        if (folder_id && folder_id->to_string() == folder.filename().string()) {
          auto metadata = load_directory_at(root,
              type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo,
              *folder_id);
          if (failure(metadata)) return *failure(metadata);
          if (auto checked = visit(folder); failure(checked)) return *failure(checked);
        } else {
          return fail(StorageErrorCode::RecoveryRequired, folder,
                      "Unexpected folder in managed module");
        }
      }
      item.increment(ec);
    }
    if (ec) return fail(StorageErrorCode::IoError, base, "Could not scan directories", ec);
  }
  return matches;
}

Result<fs::path> locate(const fs::path& root, EntryType type, const core::Id& id) {
  if (!valid_type(type)) {
    return fail(StorageErrorCode::InvalidEntryType, root, "Invalid entry type");
  }
  auto found = scan_for_id(root, id);
  if (failure(found)) return *failure(found);
  const auto& matches = std::get<std::vector<Match>>(found);
  if (matches.empty()) {
    return fail(StorageErrorCode::IoError, root, "Entry ID was not found",
                std::make_error_code(std::errc::no_such_file_or_directory));
  }
  if (matches.size() != 1 || matches.front().type != type) {
    return fail(StorageErrorCode::DuplicateId, root, "Entry ID conflicts with another path or type");
  }
  return matches.front().path;
}

Result<std::monostate> unique_id(const fs::path& root, const core::Id& id) {
  auto found = scan_for_id(root, id);
  if (failure(found)) return *failure(found);
  if (!std::get<std::vector<Match>>(found).empty()) {
    return fail(StorageErrorCode::DuplicateId, root, "Entry ID already exists");
  }
  for (auto type : {EntryType::Memo, EntryType::Todo}) {
    const auto folder = root / module_name(type) / id.to_string();
    if (auto checked = safe_path(root, folder, true); failure(checked)) return *failure(checked);
    std::error_code ec;
    if (fs::exists(folder, ec)) {
      return fail(StorageErrorCode::DuplicateId, folder, "ID already names a Directory");
    }
    if (ec) return fail(StorageErrorCode::IoError, folder, "Could not inspect Directory ID", ec);
  }
  return std::monostate{};
}

Result<core::Directory::Snapshot> load_directory_at(
    const fs::path& root, DirectoryKind kind, const core::Id& id) {
  const auto folder = root / module_name(entry_type(kind)) / id.to_string();
  if (auto checked = safe_path(root, folder); failure(checked)) return *failure(checked);
  std::error_code ec;
  if (!fs::is_directory(folder, ec) || ec) {
    return fail(StorageErrorCode::InvalidPath, folder, "Directory folder is missing", ec);
  }
  const auto metadata = folder / ".folio-dir.toml";
  auto text = read_text(root, metadata);
  if (failure(text)) return *failure(text);
  auto snapshot = decode<core::Directory::Snapshot>(
      std::get<std::string>(text), metadata, kind);
  if (failure(snapshot)) return *failure(snapshot);
  if (std::get<core::Directory::Snapshot>(snapshot).id != id) {
    return fail(StorageErrorCode::IdentityMismatch, metadata,
                "Directory metadata ID does not match its folder");
  }
  if (std::get<core::Directory::Snapshot>(snapshot).kind != kind) {
    return fail(StorageErrorCode::IdentityMismatch, metadata,
                "Directory metadata kind does not match its module");
  }
  return snapshot;
}

template <class Snapshot>
Result<Snapshot> load_at(const fs::path& root, EntryType type, const fs::path& path) {
  const auto extension = type == EntryType::Todo ? ".toml" : ".md";
  if (path.extension() != extension || path.filename() == ".folio-dir.toml") {
    return fail(StorageErrorCode::InvalidPath, path, "Path is not a formal entry file");
  }
  auto text = read_text(root, path);
  if (failure(text)) return *failure(text);
  auto snapshot = decode<Snapshot>(std::get<std::string>(text), path);
  if (failure(snapshot)) return *failure(snapshot);
  const auto& value = std::get<Snapshot>(snapshot);
  if (value.privacy != core::Privacy::Normal) {
    return fail(StorageErrorCode::InvalidPath, path, "Non-Normal entry in Normal repository");
  }
  const auto relative = path.lexically_relative(root);
  const auto module = module_name(type);
  if (relative.begin() == relative.end() || *relative.begin() != module) {
    return fail(StorageErrorCode::InvalidPath, path, "Entry is outside its module");
  }
  if constexpr (std::is_same_v<Snapshot, core::Log::Snapshot>) {
    if (path.parent_path() != root / module) {
      return fail(StorageErrorCode::InvalidPath, path, "Logs must be flat");
    }
  } else {
    const auto& directory_id = value.directory_id;
    const auto expected_parent = directory_id
        ? root / module / directory_id->to_string() : root / module;
    if (path.parent_path() != expected_parent) {
      return fail(StorageErrorCode::IdentityMismatch, path,
                  "Entry directory reference does not match its physical folder");
    }
    if (directory_id) {
      auto directory = load_directory_at(root,
          type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo,
          *directory_id);
      if (failure(directory)) return *failure(directory);
    }
  }
  return snapshot;
}

Result<core::Id> checked_entry_id(const fs::path& root, EntryType type,
                                   const fs::path& path) {
  switch (type) {
    case EntryType::Log: {
      auto loaded = load_at<core::Log::Snapshot>(root, type, path);
      if (failure(loaded)) return *failure(loaded);
      return std::get<core::Log::Snapshot>(loaded).id;
    }
    case EntryType::Memo: {
      auto loaded = load_at<core::Memo::Snapshot>(root, type, path);
      if (failure(loaded)) return *failure(loaded);
      return std::get<core::Memo::Snapshot>(loaded).id;
    }
    case EntryType::Todo: {
      auto loaded = load_at<core::Todo::Snapshot>(root, type, path);
      if (failure(loaded)) return *failure(loaded);
      return std::get<core::Todo::Snapshot>(loaded).id;
    }
  }
  return fail(StorageErrorCode::InvalidEntryType, path, "Invalid entry type");
}

Result<fs::path> exact_entry_path(const fs::path& root, EntryType type,
                                  const fs::path& relative_path) {
  if (!valid_type(type)) {
    return fail(StorageErrorCode::InvalidEntryType, relative_path, "Invalid entry type");
  }
  if (relative_path.empty() || relative_path.is_absolute() ||
      relative_path != relative_path.lexically_normal() ||
      relative_path.filename().empty()) {
    return fail(StorageErrorCode::InvalidPath, relative_path, "Expected a safe relative locator");
  }
  std::vector<fs::path> parts;
  for (const auto& component : relative_path) {
    if (component == "." || component == "..") {
      return fail(StorageErrorCode::InvalidPath, relative_path, "Locator contains traversal");
    }
    parts.push_back(component);
  }
  if (parts.size() < 2 || parts[0] != module_name(type) ||
      (type == EntryType::Log ? parts.size() != 2 : parts.size() > 3)) {
    return fail(StorageErrorCode::InvalidPath, relative_path,
                "Locator does not follow the entry module layout");
  }
  if (parts.size() == 3) {
    const auto folder_id = core::Id::parse(parts[1].string());
    if (!folder_id || folder_id->to_string() != parts[1].string()) {
      return fail(StorageErrorCode::InvalidPath, relative_path,
                  "Locator Directory folder is not a canonical UUID");
    }
  }
  const auto extension = type == EntryType::Todo ? ".toml" : ".md";
  if (relative_path.extension() != extension ||
      relative_path.filename() == ".folio-dir.toml") {
    return fail(StorageErrorCode::InvalidPath, relative_path,
                "Locator is not a formal entry file");
  }
  const auto absolute = root / relative_path;
  if (auto checked = safe_path(root, absolute, true); failure(checked)) {
    return *failure(checked);
  }
  std::error_code ec;
  const auto status = fs::symlink_status(absolute, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      (!ec && status.type() == fs::file_type::not_found)) {
    return fail(StorageErrorCode::StaleLocator, relative_path,
                "Entry locator no longer exists", ec);
  }
  if (ec) return fail(StorageErrorCode::IoError, relative_path,
                      "Could not inspect entry locator", ec);
  if (!fs::is_regular_file(status)) {
    return fail(StorageErrorCode::InvalidPath, relative_path,
                "Entry locator is not a regular file");
  }
  return absolute;
}

Result<fs::path> resolve_entry_path(const fs::path& root, EntryType type,
                                     const core::Id& id,
                                     const std::optional<fs::path>& relative_path) {
  return relative_path ? exact_entry_path(root, type, *relative_path)
                       : locate(root, type, id);
}

template <class Snapshot>
Result<Snapshot> load_by_id(const fs::path& root, EntryType type, const core::Id& id,
                            const std::optional<fs::path>& relative_path) {
  auto path = resolve_entry_path(root, type, id, relative_path);
  if (failure(path)) return *failure(path);
  auto loaded = load_at<Snapshot>(root, type, std::get<fs::path>(path));
  if (failure(loaded)) {
    const auto& problem = *failure(loaded);
    if (relative_path && problem.path == std::get<fs::path>(path) &&
        problem.system_error == std::errc::no_such_file_or_directory) {
      return fail(StorageErrorCode::StaleLocator, *relative_path,
                  "Entry locator no longer exists", problem.system_error);
    }
    return problem;
  }
  if (std::get<Snapshot>(loaded).id != id) {
    return fail(relative_path ? StorageErrorCode::StaleLocator
                              : StorageErrorCode::IdentityMismatch,
                relative_path ? *relative_path : std::get<fs::path>(path),
                "Stored entry ID differs from requested ID");
  }
  return loaded;
}

template <class Snapshot>
Result<std::monostate> create_entry(const fs::path& root, EntryType type,
                                    const Snapshot& snapshot) {
  auto name = filename(type, snapshot.id, snapshot.created);
  if (failure(name)) return *failure(name);
  fs::path parent = root / module_name(type);
  if constexpr (!std::is_same_v<Snapshot, core::Log::Snapshot>) {
    if (snapshot.directory_id) {
      auto directory = load_directory_at(root,
          type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo,
          *snapshot.directory_id);
      if (failure(directory)) return *failure(directory);
      parent /= snapshot.directory_id->to_string();
    }
  }
  const auto path = parent / std::get<std::string>(name);
  auto encoded = checked_encoding(snapshot, path);
  if (failure(encoded)) return *failure(encoded);
  if (auto checked = unique_id(root, snapshot.id); failure(checked)) return *failure(checked);
  // TODO(Phase 5): journal and sync root/module bootstrap. If the atomic
  // create fails, newly made empty parent directories may remain.
  if (auto checked = ensure_directory(root, parent); failure(checked)) return *failure(checked);
  return create_file_atomic(path, std::get<std::string>(encoded));
}

template <class Snapshot>
Result<std::monostate> update_entry(const fs::path& root, EntryType type,
                                    const Snapshot& snapshot,
                                    const std::optional<fs::path>& relative_path) {
  auto path = resolve_entry_path(root, type, snapshot.id, relative_path);
  if (failure(path)) return *failure(path);
  const auto& destination = std::get<fs::path>(path);
  auto existing = load_at<Snapshot>(root, type, destination);
  if (failure(existing)) {
    const auto& problem = *failure(existing);
    if (relative_path && problem.path == destination &&
        problem.system_error == std::errc::no_such_file_or_directory) {
      return fail(StorageErrorCode::StaleLocator, *relative_path,
                  "Entry locator no longer exists", problem.system_error);
    }
    return problem;
  }
  const auto& prior = std::get<Snapshot>(existing);
  if (prior.id != snapshot.id) {
    return fail(relative_path ? StorageErrorCode::StaleLocator
                              : StorageErrorCode::IdentityMismatch,
                relative_path ? *relative_path : destination,
                "Entry locator contains a different stored UUID");
  }
  if (prior.created.timestamp != snapshot.created.timestamp ||
      prior.created.timezone != snapshot.created.timezone ||
      prior.created.location != snapshot.created.location ||
      prior.privacy != snapshot.privacy) {
    return fail(StorageErrorCode::IdentityMismatch, destination,
                "Update changed immutable entry identity, creation context, or privacy");
  }
  if constexpr (!std::is_same_v<Snapshot, core::Log::Snapshot>) {
    if (prior.directory_id != snapshot.directory_id) {
      return fail(StorageErrorCode::InvalidPath, destination,
                  "Use move to change an entry's Directory");
    }
  }
  auto encoded = checked_encoding(snapshot, destination);
  if (failure(encoded)) return *failure(encoded);
  return replace_file_atomic(destination, std::get<std::string>(encoded));
}

#ifndef _WIN32
Result<std::monostate> sync_parent(const fs::path& path) {
  const int descriptor = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor == -1) {
    return fail(StorageErrorCode::DurabilityFailure, path,
                "Could not open source directory for move sync", {errno, std::generic_category()}, true);
  }
  int status;
  do { status = fsync(descriptor); } while (status == -1 && errno == EINTR);
  const auto ec = status == -1 ? std::error_code{errno, std::generic_category()} : std::error_code{};
  close(descriptor);
  if (status == -1) {
    return fail(StorageErrorCode::DurabilityFailure, path,
                "Source removal completed but directory sync failed", ec, true);
  }
  return std::monostate{};
}
#endif

template <class Snapshot>
Result<std::monostate> move_entry(const fs::path& root, EntryType type,
                                  const core::Id& id, std::optional<core::Id> directory_id,
                                  core::TimestampContext modified) {
  auto source = locate(root, type, id);
  if (failure(source)) return *failure(source);
  const auto old_path = std::get<fs::path>(source);
  auto loaded = load_at<Snapshot>(root, type, old_path);
  if (failure(loaded)) return *failure(loaded);
  auto snapshot = std::get<Snapshot>(std::move(loaded));
  if (snapshot.directory_id == directory_id) return std::monostate{};
  fs::path parent = root / module_name(type);
  if (directory_id) {
    auto directory = load_directory_at(root,
        type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo, *directory_id);
    if (failure(directory)) return *failure(directory);
    parent /= directory_id->to_string();
  }
  snapshot.directory_id = std::move(directory_id);
  snapshot.modified = std::move(modified);
  const auto destination = parent / old_path.filename();
  auto encoded = checked_encoding(snapshot, destination);
  if (failure(encoded)) return *failure(encoded);
  if (auto checked = safe_path(root, destination, true); failure(checked)) return *failure(checked);
  // TODO(Phase 5): journal this two-path operation. Destination is created
  // first, so an interruption may leave both versions and needs recovery.
  auto created = create_file_atomic(destination, std::get<std::string>(encoded));
  if (failure(created)) return *failure(created);
  std::error_code ec;
  if (!fs::remove(old_path, ec) || ec) {
    return fail(StorageErrorCode::IoError, old_path,
                "Destination created but source removal failed; reconciliation required", ec, true);
  }
#ifndef _WIN32
  return sync_parent(old_path);
#else
  return std::monostate{};
#endif
}

}  // namespace

NormalRepository::NormalRepository(fs::path normal_root) : root_(std::move(normal_root)) {}

Result<std::monostate> NormalRepository::create_log(const core::Log::Snapshot& snapshot) {
  return create_entry(root_, EntryType::Log, snapshot);
}
Result<std::monostate> NormalRepository::create_memo(const core::Memo::Snapshot& snapshot) {
  return create_entry(root_, EntryType::Memo, snapshot);
}
Result<std::monostate> NormalRepository::create_todo(const core::Todo::Snapshot& snapshot) {
  return create_entry(root_, EntryType::Todo, snapshot);
}
Result<core::Log::Snapshot> NormalRepository::load_log(
    const core::Id& id, std::optional<fs::path> relative_path) const {
  return load_by_id<core::Log::Snapshot>(root_, EntryType::Log, id, relative_path);
}
Result<core::Memo::Snapshot> NormalRepository::load_memo(
    const core::Id& id, std::optional<fs::path> relative_path) const {
  return load_by_id<core::Memo::Snapshot>(root_, EntryType::Memo, id, relative_path);
}
Result<core::Todo::Snapshot> NormalRepository::load_todo(
    const core::Id& id, std::optional<fs::path> relative_path) const {
  return load_by_id<core::Todo::Snapshot>(root_, EntryType::Todo, id, relative_path);
}
Result<std::monostate> NormalRepository::update_log(
    const core::Log::Snapshot& snapshot, std::optional<fs::path> relative_path) {
  return update_entry(root_, EntryType::Log, snapshot, relative_path);
}
Result<std::monostate> NormalRepository::update_memo(
    const core::Memo::Snapshot& snapshot, std::optional<fs::path> relative_path) {
  return update_entry(root_, EntryType::Memo, snapshot, relative_path);
}
Result<std::monostate> NormalRepository::update_todo(
    const core::Todo::Snapshot& snapshot, std::optional<fs::path> relative_path) {
  return update_entry(root_, EntryType::Todo, snapshot, relative_path);
}

Result<std::monostate> NormalRepository::create_directory(
    DirectoryKind kind, const core::Directory::Snapshot& snapshot) {
  if (!valid_kind(kind)) {
    return fail(StorageErrorCode::InvalidEntryType, root_, "Invalid Directory kind");
  }
  if (snapshot.kind != kind) {
    return fail(StorageErrorCode::InvalidMetadata, root_,
                "Directory snapshot kind does not match target module");
  }
  const auto folder = root_ / module_name(entry_type(kind)) / snapshot.id.to_string();
  const auto metadata = folder / ".folio-dir.toml";
  auto encoded = checked_encoding(snapshot, metadata);
  if (failure(encoded)) return *failure(encoded);
  if (auto checked = unique_id(root_, snapshot.id); failure(checked)) return *failure(checked);
  // TODO(Phase 5): journal and sync root/module bootstrap as part of
  // Directory creation; failure can leave empty parent directories.
  if (auto checked = ensure_directory(root_, folder.parent_path()); failure(checked)) {
    return *failure(checked);
  }
  std::error_code ec;
  if (!fs::create_directory(folder, ec) || ec) {
    return fail(StorageErrorCode::IoError, folder, "Could not create Directory folder", ec);
  }
  // TODO(Phase 5): journal folder creation and metadata publication. A
  // failed metadata write leaves an identifiable incomplete UUID folder.
  return create_file_atomic(metadata, std::get<std::string>(encoded));
}

Result<core::Directory::Snapshot> NormalRepository::load_directory(
    DirectoryKind kind, const core::Id& id) const {
  if (!valid_kind(kind)) {
    return fail(StorageErrorCode::InvalidEntryType, root_, "Invalid Directory kind");
  }
  auto checked = scan_for_id(root_, id);
  if (failure(checked)) return *failure(checked);
  return load_directory_at(root_, kind, id);
}

Result<std::monostate> NormalRepository::rename_directory(
    DirectoryKind kind, const core::Id& id, std::string name) {
  if (!valid_kind(kind)) {
    return fail(StorageErrorCode::InvalidEntryType, root_, "Invalid Directory kind");
  }
  auto checked = scan_for_id(root_, id);
  if (failure(checked)) return *failure(checked);
  auto prior = load_directory_at(root_, kind, id);
  if (failure(prior)) return *failure(prior);
  const auto metadata = root_ / module_name(entry_type(kind)) / id.to_string() /
      ".folio-dir.toml";
  auto encoded = checked_encoding(core::Directory::Snapshot{id, kind, std::move(name)}, metadata);
  if (failure(encoded)) return *failure(encoded);
  return replace_file_atomic(metadata, std::get<std::string>(encoded));
}

Result<std::monostate> NormalRepository::move_memo(
    const core::Id& id, std::optional<core::Id> directory_id,
    core::TimestampContext modified) {
  return move_entry<core::Memo::Snapshot>(root_, EntryType::Memo, id,
                                           std::move(directory_id), std::move(modified));
}
Result<std::monostate> NormalRepository::move_todo(
    const core::Id& id, std::optional<core::Id> directory_id,
    core::TimestampContext modified) {
  return move_entry<core::Todo::Snapshot>(root_, EntryType::Todo, id,
                                           std::move(directory_id), std::move(modified));
}

Result<fs::path> NormalRepository::locate_entry(EntryType type, const core::Id& id) const {
  auto located = locate(root_, type, id);
  if (failure(located)) return *failure(located);
  const auto path = std::get<fs::path>(located);
  switch (type) {
    case EntryType::Log: {
      auto loaded = load_at<core::Log::Snapshot>(root_, type, path);
      if (failure(loaded)) return *failure(loaded);
      break;
    }
    case EntryType::Memo: {
      auto loaded = load_at<core::Memo::Snapshot>(root_, type, path);
      if (failure(loaded)) return *failure(loaded);
      break;
    }
    case EntryType::Todo: {
      auto loaded = load_at<core::Todo::Snapshot>(root_, type, path);
      if (failure(loaded)) return *failure(loaded);
      break;
    }
  }
  return path.lexically_relative(root_);
}

Result<fs::path> NormalRepository::locate_entry(
    EntryType type, const fs::path& relative_path) const {
  if (!valid_type(type)) {
    return fail(StorageErrorCode::InvalidEntryType, relative_path, "Invalid entry type");
  }
  if (relative_path.empty() || relative_path.is_absolute() ||
      relative_path != relative_path.lexically_normal()) {
    return fail(StorageErrorCode::InvalidPath, relative_path, "Expected a safe relative locator");
  }
  for (const auto& component : relative_path) {
    if (component == "." || component == "..") {
      return fail(StorageErrorCode::InvalidPath, relative_path, "Locator contains traversal");
    }
  }
  const auto absolute = root_ / relative_path;
  auto stored_id = checked_entry_id(root_, type, absolute);
  if (failure(stored_id)) return *failure(stored_id);
  auto located = locate_entry(type, std::get<core::Id>(stored_id));
  if (failure(located)) return *failure(located);
  if (std::get<fs::path>(located) != relative_path) {
    return fail(StorageErrorCode::IdentityMismatch, relative_path,
                "Locator does not match the entry's physical path");
  }
  return located;
}

Result<NormalRepositoryScanResult> NormalRepository::scan() const {
  NormalRepositoryScanResult inventory;
  if (auto checked = safe_path(root_, root_, true); failure(checked)) return *failure(checked);
  std::error_code ec;
  const auto root_status = fs::symlink_status(root_, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      root_status.type() == fs::file_type::not_found) return inventory;
  if (ec) return fail(StorageErrorCode::IoError, root_, "Could not inspect Normal root", ec);
  if (!fs::is_directory(root_status))
    return fail(StorageErrorCode::InvalidPath, root_, "Normal root is not a directory");

  std::set<std::string> entry_ids;
  std::set<std::string> directory_ids;
  auto note = [&](const fs::path& path, std::string message) {
    inventory.diagnostics.push_back({path.lexically_relative(root_), std::move(message)});
  };
  auto reserved_temp = [](const fs::path& path) {
    const auto name = path.filename().string();
    if (name.size() < 3 || name.front() != '.') return false;
    const auto marker = name.rfind(".tmp-");
    if (marker == std::string::npos || marker <= 1) return false;
    const auto suffix = name.substr(marker + 5);
    const auto id = core::Id::parse(suffix);
    return id && id->to_string() == suffix;
  };
  auto unknown_folder_has_managed_file = [&](const fs::path& folder,
                                             EntryType type) -> Result<bool> {
    std::error_code folder_ec;
    fs::directory_iterator child{folder, folder_ec};
    const fs::directory_iterator end;
    while (!folder_ec && child != end) {
      const auto path = child->path();
      if (path.filename() == ".folio-dir.toml" ||
          path.extension() == (type == EntryType::Todo ? ".toml" : ".md")) return true;
      child.increment(folder_ec);
    }
    if (folder_ec) return fail(StorageErrorCode::IoError, folder,
                               "Could not inspect unrelated folder", folder_ec);
    return false;
  };
  auto visit_files = [&](EntryType type, const fs::path& folder,
                         bool in_directory) -> Result<std::monostate> {
    std::error_code iter_ec;
    fs::directory_iterator item{folder, iter_ec};
    const fs::directory_iterator end;
    while (!iter_ec && item != end) {
      const auto path = item->path();
      const auto status = item->symlink_status(iter_ec);
      if (iter_ec) break;
      if (in_directory && path.filename() == ".folio-dir.toml") {
        if (!fs::is_regular_file(status))
          return fail(StorageErrorCode::InvalidPath, path, "Directory metadata is not a regular file");
      } else if (reserved_temp(path) && fs::is_regular_file(status)) {
        // Recovery owns reserved atomic temporary artifacts.
      } else if (reserved_temp(path) && fs::is_symlink(status)) {
        return fail(StorageErrorCode::InvalidPath, path, "Symlink at reserved temporary path");
      } else if (path.filename() == ".folio-dir.toml") {
        return fail(StorageErrorCode::InvalidPath, path, "Misplaced Directory metadata");
      } else if (fs::is_directory(status)) {
        if (in_directory)
          return fail(StorageErrorCode::RecoveryRequired, path, "Unexpected nested folder");
        if (type == EntryType::Log) {
          auto managed = unknown_folder_has_managed_file(path, type);
          if (failure(managed)) return *failure(managed);
          if (std::get<bool>(managed))
            return fail(StorageErrorCode::RecoveryRequired, path,
                        "Unexpected folder contains formal Log files");
          note(path, "Unrelated folder was not traversed");
        }
        // A module-root Directory candidate is processed separately below.
      } else {
        if (fs::is_symlink(status) && !in_directory && type != EntryType::Log) {
          const auto id = core::Id::parse(path.filename().string());
          if (id && id->to_string() == path.filename().string())
            return fail(StorageErrorCode::InvalidPath, path,
                        "Symlink at managed Directory path");
        }
        const auto extension = type == EntryType::Todo ? ".toml" : ".md";
        if (path.extension() == extension) {
          if (!fs::is_regular_file(status))
            return fail(StorageErrorCode::InvalidPath, path, "Formal Entry is not a regular file");
          if (auto checked = safe_path(root_, path); failure(checked)) return *failure(checked);
          const auto relative = path.lexically_relative(root_);
          if (type == EntryType::Log) {
            auto loaded = load_at<core::Log::Snapshot>(root_, type, path);
            if (failure(loaded)) return *failure(loaded);
            auto value = std::get<core::Log::Snapshot>(std::move(loaded));
            if (!entry_ids.insert(value.id.to_string()).second)
              return fail(StorageErrorCode::DuplicateId, path, "Duplicate stored Entry UUID");
            inventory.logs.push_back({value.id, relative, value.privacy,
                                      std::move(value.created), std::move(value.modified),
                                      std::move(value.title), std::move(value.tags),
                                      std::move(value.log_date)});
          } else if (type == EntryType::Memo) {
            auto loaded = load_at<core::Memo::Snapshot>(root_, type, path);
            if (failure(loaded)) return *failure(loaded);
            auto value = std::get<core::Memo::Snapshot>(std::move(loaded));
            if (!entry_ids.insert(value.id.to_string()).second)
              return fail(StorageErrorCode::DuplicateId, path, "Duplicate stored Entry UUID");
            inventory.memos.push_back({value.id, relative, value.privacy,
                                       std::move(value.created), std::move(value.modified),
                                       std::move(value.title), std::move(value.tags),
                                       std::move(value.directory_id)});
          } else {
            auto loaded = load_at<core::Todo::Snapshot>(root_, type, path);
            if (failure(loaded)) return *failure(loaded);
            auto value = std::get<core::Todo::Snapshot>(std::move(loaded));
            if (!entry_ids.insert(value.id.to_string()).second)
              return fail(StorageErrorCode::DuplicateId, path, "Duplicate stored Entry UUID");
            inventory.todos.push_back({value.id, relative, value.privacy,
                                       std::move(value.created), std::move(value.modified),
                                       std::move(value.title), value.status, value.priority,
                                       std::move(value.due_date), std::move(value.directory_id)});
          }
        } else {
          note(path, "Unrelated object was not parsed");
        }
      }
      item.increment(iter_ec);
    }
    if (iter_ec) return fail(StorageErrorCode::IoError, folder, "Could not enumerate folder", iter_ec);
    return std::monostate{};
  };

  fs::directory_iterator root_item{root_, ec};
  const fs::directory_iterator end;
  while (!ec && root_item != end) {
    const auto path = root_item->path();
    const auto name = path.filename();
    if (name != "logs" && name != "memos" && name != "todos") note(path, "Unrelated root object was not parsed");
    root_item.increment(ec);
  }
  if (ec) return fail(StorageErrorCode::IoError, root_, "Could not enumerate Normal root", ec);

  for (auto type : {EntryType::Log, EntryType::Memo, EntryType::Todo}) {
    const auto base = root_ / module_name(type);
    if (auto checked = safe_path(root_, base, true); failure(checked)) return *failure(checked);
    const auto status = fs::symlink_status(base, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found) { ec.clear(); continue; }
    if (ec) return fail(StorageErrorCode::IoError, base, "Could not inspect module", ec);
    if (!fs::is_directory(status))
      return fail(StorageErrorCode::InvalidPath, base, "Module is not a directory");
    if (auto visited = visit_files(type, base, false); failure(visited)) return *failure(visited);
    if (type == EntryType::Log) continue;
    fs::directory_iterator item{base, ec};
    while (!ec && item != end) {
      const auto folder = item->path();
      const auto folder_status = item->symlink_status(ec);
      if (ec) break;
      if (fs::is_directory(folder_status)) {
        const auto folder_id = core::Id::parse(folder.filename().string());
        if (!folder_id || folder_id->to_string() != folder.filename().string()) {
          auto managed = unknown_folder_has_managed_file(folder, type);
          if (failure(managed)) return *failure(managed);
          if (std::get<bool>(managed))
            return fail(StorageErrorCode::RecoveryRequired, folder,
                        "Unexpected folder contains formal Entry files or Directory metadata");
          note(folder, "Unrelated folder was not traversed");
        } else {
          const auto kind = type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
          auto loaded = load_directory_at(root_, kind, *folder_id);
          if (failure(loaded)) return *failure(loaded);
          auto value = std::get<core::Directory::Snapshot>(std::move(loaded));
          if (!directory_ids.insert(value.id.to_string()).second)
            return fail(StorageErrorCode::DuplicateId, folder, "Duplicate Directory UUID");
          inventory.directories.push_back(metadata_from(value, folder.lexically_relative(root_)));
          if (auto visited = visit_files(type, folder, true); failure(visited)) return *failure(visited);
        }
      }
      item.increment(ec);
    }
    if (ec) return fail(StorageErrorCode::IoError, base, "Could not enumerate Directories", ec);
  }
  auto by_path = [](const auto& a, const auto& b) {
    return a.relative_path < b.relative_path;
  };
  std::sort(inventory.logs.begin(), inventory.logs.end(), by_path);
  std::sort(inventory.memos.begin(), inventory.memos.end(), by_path);
  std::sort(inventory.todos.begin(), inventory.todos.end(), by_path);
  std::sort(inventory.directories.begin(), inventory.directories.end(), [](const auto& a, const auto& b) {
    return a.relative_path < b.relative_path;
  });
  std::sort(inventory.diagnostics.begin(), inventory.diagnostics.end(), [](const auto& a, const auto& b) {
    return a.path < b.path;
  });
  return inventory;
}

}  // namespace folio::core::storage
