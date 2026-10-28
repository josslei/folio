#include "storage/metadata_catalog.hpp"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace folio::core::storage {
namespace {
::folio::core::internal::Result<std::monostate, StorageError> failure(StorageErrorCode code, const std::filesystem::path& path,
                std::string message) {
  return StorageError{code, path, std::move(message), {}, false};
}

bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) { ++i; continue; }
    unsigned count = 0;
    std::uint32_t code = 0;
    if ((first & 0xe0) == 0xc0) { count = 2; code = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { count = 3; code = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { count = 4; code = first & 0x07; }
    else return false;
    if (i + count > text.size()) return false;
    for (unsigned j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
        (count == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) return false;
    i += count;
  }
  return true;
}

bool valid_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory() || path.native().find(
          std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) return false;
  for (const auto& component : path) {
    if (component.empty() || component == "." || component == "..") return false;
  }
  const auto bytes = path.generic_u8string();
  return valid_utf8(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool valid_privacy(Privacy p) {
  return p == Privacy::Normal || p == Privacy::Protected || p == Privacy::Hidden;
}
bool valid_kind(DirectoryKind k) { return k == DirectoryKind::Memo || k == DirectoryKind::Todo; }
bool valid_status(TodoStatus s) { return s == TodoStatus::Pending || s == TodoStatus::Completed; }
bool valid_priority(TodoPriority p) {
  return p == TodoPriority::None || p == TodoPriority::Low ||
         p == TodoPriority::Medium || p == TodoPriority::High;
}
bool valid_context(const TimestampContext& c) {
  return valid_utf8(c.timezone) && (!c.location || valid_utf8(*c.location));
}
bool valid_context(const DateContext& c) {
  return c.date.ok() && valid_utf8(c.timezone) &&
         (!c.location || valid_utf8(*c.location));
}
const Id& entry_id(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> const Id& { return value.id; }, e);
}
const std::filesystem::path& entry_path(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> const std::filesystem::path& {
    return value.relative_path;
  }, e);
}
std::optional<Id> entry_directory(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> std::optional<Id> {
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, LogMetadata>) return std::nullopt;
    else return value.directory_id;
  }, e);
}
Privacy entry_privacy(const EntryMetadata& e) {
  return std::visit([](const auto& value) { return value.privacy; }, e);
}
const TimestampContext& entry_created(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> const TimestampContext& { return value.created; }, e);
}
const TimestampContext& entry_modified(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> const TimestampContext& { return value.modified; }, e);
}
const std::string& entry_title(const EntryMetadata& e) {
  return std::visit([](const auto& value) -> const std::string& {
    return value.title;
  }, e);
}

bool valid_entry(const EntryMetadata& e) {
  if (!valid_path(entry_path(e)) || !valid_privacy(entry_privacy(e)) ||
      !valid_context(entry_created(e)) || !valid_context(entry_modified(e)) ||
      !valid_utf8(entry_title(e))) return false;
  return std::visit([](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, TodoMetadata>) {
      if (!valid_status(value.status) || !valid_priority(value.priority)) return false;
      if (value.due_date && !std::visit([](const auto& due) { return valid_context(due); },
                                         *value.due_date)) return false;
    } else {
      for (const auto& tag : value.tags) if (!valid_utf8(tag.value)) return false;
      if constexpr (std::is_same_v<T, LogMetadata>)
        if (!valid_context(value.log_date)) return false;
    }
    return true;
  }, e);
}

bool contains_ascii_folded(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                     [&](char a, char b) {
                       return lower(static_cast<unsigned char>(a)) ==
                              lower(static_cast<unsigned char>(b));
                     }) != haystack.end();
}

template <class T>
bool in_range(const T& value, const InclusiveRange<T>& range) {
  return (!range.from || !(value < *range.from)) &&
         (!range.through || !(*range.through < value));
}
bool in_date_range(const std::chrono::year_month_day& date,
                   const InclusiveRange<std::chrono::year_month_day>& range) {
  const std::chrono::sys_days day{date};
  return (!range.from || day >= std::chrono::sys_days{*range.from}) &&
         (!range.through || day <= std::chrono::sys_days{*range.through});
}
int cmp_string(std::string_view a, std::string_view b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}
template <class T>
int cmp_scalar(const T& a, const T& b) { return a < b ? -1 : (b < a ? 1 : 0); }
int cmp_due(const DueDate& a, const DueDate& b) {
  if (a.index() != b.index()) return cmp_scalar(a.index(), b.index());
  if (const auto* date = std::get_if<DateContext>(&a)) {
    return cmp_scalar(std::chrono::sys_days{date->date},
                      std::chrono::sys_days{std::get<DateContext>(b).date});
  }
  return cmp_scalar(std::get<TimestampContext>(a).timestamp,
                    std::get<TimestampContext>(b).timestamp);
}
// -2 means an inapplicable or absent key. Callers keep it last in both directions.
int compare_key(const EntryMetadata& a, const EntryMetadata& b, MetadataSort sort) {
  switch (sort) {
    case MetadataSort::Id: return cmp_string(entry_id(a).to_string(), entry_id(b).to_string());
    case MetadataSort::Type: return cmp_scalar(static_cast<int>(entry_type(a)), static_cast<int>(entry_type(b)));
    case MetadataSort::Created: return cmp_scalar(entry_created(a).timestamp, entry_created(b).timestamp);
    case MetadataSort::Modified: return cmp_scalar(entry_modified(a).timestamp, entry_modified(b).timestamp);
    case MetadataSort::Title: return cmp_string(entry_title(a), entry_title(b));
    case MetadataSort::Privacy: return cmp_scalar(static_cast<int>(entry_privacy(a)), static_cast<int>(entry_privacy(b)));
    case MetadataSort::LogDate: {
      const auto* x = std::get_if<LogMetadata>(&a);
      const auto* y = std::get_if<LogMetadata>(&b);
      if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
      return cmp_scalar(std::chrono::sys_days{x->log_date.date},
                        std::chrono::sys_days{y->log_date.date});
    }
    case MetadataSort::DueDate: {
      const auto* x = std::get_if<TodoMetadata>(&a);
      const auto* y = std::get_if<TodoMetadata>(&b);
      const bool px = x && x->due_date;
      const bool py = y && y->due_date;
      if (!px || !py) return !px && !py ? 0 : (!px ? -2 : 2);
      return cmp_due(*x->due_date, *y->due_date);
    }
    case MetadataSort::Status: {
      const auto* x = std::get_if<TodoMetadata>(&a);
      const auto* y = std::get_if<TodoMetadata>(&b);
      if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
      return cmp_scalar(static_cast<int>(x->status), static_cast<int>(y->status));
    }
    case MetadataSort::Priority: {
      const auto* x = std::get_if<TodoMetadata>(&a);
      const auto* y = std::get_if<TodoMetadata>(&b);
      if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
      return cmp_scalar(static_cast<int>(x->priority), static_cast<int>(y->priority));
    }
  }
  return 0;
}

template <class T>
bool matches(const T& e, const CatalogQuery& q) {
  constexpr bool log = std::is_same_v<T, LogMetadata>;
  constexpr bool memo = std::is_same_v<T, MemoMetadata>;
  constexpr bool todo = std::is_same_v<T, TodoMetadata>;
  constexpr EntryType type = log ? EntryType::Log : (memo ? EntryType::Memo : EntryType::Todo);
  if (q.type && *q.type != type) return false;
  if (q.directory) {
    if constexpr (log) {
      if (*q.directory) return false;
    } else if (*q.directory != e.directory_id) return false;
  }
  if (q.privacy && *q.privacy != e.privacy) return false;
  if (!contains_ascii_folded(e.title, q.text)) return false;
  if (!in_range(e.created.timestamp, q.created) ||
      !in_range(e.modified.timestamp, q.modified)) return false;
  if (q.tag) {
    if constexpr (todo) return false;
    else if (!std::any_of(e.tags.begin(), e.tags.end(), [&](const Tag& tag) {
      return contains_ascii_folded(tag.value, q.tag->value);
    })) return false;
  }
  if (q.log_date.from || q.log_date.through) {
    if constexpr (log) {
      if (!in_date_range(e.log_date.date, q.log_date)) return false;
    } else return false;
  }
  if (q.todo_status || q.todo_priority || q.has_due_date || q.due_kind ||
      q.due_from || q.due_through) {
    if constexpr (!todo) return false;
    else {
      if (q.todo_status && *q.todo_status != e.status) return false;
      if (q.todo_priority && *q.todo_priority != e.priority) return false;
      if (q.has_due_date && *q.has_due_date != e.due_date.has_value()) return false;
      if (q.due_kind && (!e.due_date ||
          static_cast<std::size_t>(*q.due_kind) != e.due_date->index())) return false;
      if (q.due_from || q.due_through) {
        if (!e.due_date) return false;
        if (q.due_from && (q.due_from->index() != e.due_date->index() ||
                           cmp_due(*e.due_date, *q.due_from) < 0)) return false;
        if (q.due_through && (q.due_through->index() != e.due_date->index() ||
                              cmp_due(*e.due_date, *q.due_through) > 0)) return false;
      }
    }
  }
  return true;
}
}  // namespace

EntryType entry_type(const EntryMetadata& metadata) noexcept {
  return std::visit([](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, LogMetadata>) return EntryType::Log;
    else if constexpr (std::is_same_v<T, MemoMetadata>) return EntryType::Memo;
    else return EntryType::Todo;
  }, metadata);
}

EntryMetadata metadata_from(const Log::Snapshot& s, std::filesystem::path path) {
  return LogMetadata{s.id, std::move(path), s.privacy, s.created, s.modified,
                     s.title, s.tags, s.log_date};
}
EntryMetadata metadata_from(const Memo::Snapshot& s, std::filesystem::path path) {
  return MemoMetadata{s.id, std::move(path), s.privacy, s.created, s.modified,
                      s.title, s.tags, s.directory_id};
}
EntryMetadata metadata_from(const Todo::Snapshot& s, std::filesystem::path path) {
  return TodoMetadata{s.id, std::move(path), s.privacy, s.created, s.modified,
                      s.title, s.status, s.priority, s.due_date, s.directory_id};
}
DirectoryMetadata metadata_from(const Directory::Snapshot& s,
                                std::filesystem::path path) {
  return DirectoryMetadata{s.id, s.kind, std::move(path), s.name};
}

bool MetadataCatalog::entry_path_occupied(const std::filesystem::path& path,
                                          std::optional<Id> except) const {
  auto occupied = [&](const auto& collection) {
    return std::any_of(collection.begin(), collection.end(), [&](const auto& pair) {
      const auto& e = pair.second;
      return e.relative_path == path && (!except || e.id != *except);
    });
  };
  return occupied(logs_) || occupied(memos_) || occupied(todos_);
}

bool MetadataCatalog::directory_referenced(const DirectoryMetadata& directory) const {
  const auto it = directory_members_.find(directory.id.to_string());
  return it != directory_members_.end() && !it->second.empty();
}

std::set<std::string>& MetadataCatalog::members_for(EntryType type,
                                                    std::optional<Id> directory_id) {
  if (directory_id) return directory_members_.at(directory_id->to_string());
  switch (type) {
    case EntryType::Log: return log_roots_;
    case EntryType::Memo: return memo_roots_;
    case EntryType::Todo: return todo_roots_;
  }
  return log_roots_;
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::insert_entry(EntryMetadata m) {
  if (!valid_entry(m)) return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Invalid entry metadata");
  const Id id = entry_id(m);
  const auto key = id.to_string();
  if (find_entry(id)) return failure(StorageErrorCode::DuplicateId, entry_path(m), "Entry UUID already exists");
  if (entry_path_occupied(entry_path(m)))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& [directory_key, d] : directories_)
    if (d.relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  const auto type = entry_type(m);
  const auto directory_id = entry_directory(m);
  if (directory_id) {
    const auto found = find_directory(*directory_id);
    const auto kind = type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Missing or wrong-kind Directory");
  }
  auto& members = members_for(type, directory_id);
  members.insert(key);
  try {
    std::visit([&](auto&& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, LogMetadata>) logs_.emplace(key, std::move(value));
      else if constexpr (std::is_same_v<T, MemoMetadata>) memos_.emplace(key, std::move(value));
      else todos_.emplace(key, std::move(value));
    }, std::move(m));
  } catch (...) {
    members.erase(key);
    throw;
  }
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::update_entry(EntryMetadata m) {
  if (!valid_entry(m)) return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Invalid entry metadata");
  const Id id = entry_id(m);
  const auto key = id.to_string();
  const auto existing = find_entry(id);
  if (!existing) return failure(StorageErrorCode::NotFound, entry_path(m), "Entry UUID not found");
  const auto type = entry_type(m);
  if (entry_type(*existing) != type)
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Entry type changed");
  if (entry_path_occupied(entry_path(m), id))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& [directory_key, d] : directories_)
    if (d.relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  const auto old_directory = entry_directory(*existing);
  const auto new_directory = entry_directory(m);
  if (new_directory) {
    const auto found = find_directory(*new_directory);
    const auto kind = type == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Missing or wrong-kind Directory");
  }
  std::set<std::string>* old_members = nullptr;
  if (new_directory != old_directory) {
    old_members = &members_for(type, old_directory);
    members_for(type, new_directory).insert(key);
  }
  std::visit([&](auto&& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, LogMetadata>) {
      static_assert(std::is_nothrow_swappable_v<T>);
      std::swap(logs_.at(key), value);
    } else if constexpr (std::is_same_v<T, MemoMetadata>) {
      static_assert(std::is_nothrow_swappable_v<T>);
      std::swap(memos_.at(key), value);
    } else {
      static_assert(std::is_nothrow_swappable_v<T>);
      std::swap(todos_.at(key), value);
    }
  }, m);
  if (old_members) old_members->erase(key);
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::remove_entry(const Id& id) {
  const auto key = id.to_string();
  const auto existing = find_entry(id);
  if (!existing) return failure(StorageErrorCode::NotFound, {}, "Entry UUID not found");
  const auto type = entry_type(*existing);
  members_for(type, entry_directory(*existing)).erase(key);
  switch (type) {
    case EntryType::Log: logs_.erase(key); break;
    case EntryType::Memo: memos_.erase(key); break;
    case EntryType::Todo: todos_.erase(key); break;
  }
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::insert_directory(DirectoryMetadata m) {
  if (!valid_kind(m.kind) || !valid_path(m.relative_path) || !valid_utf8(m.name))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Invalid Directory metadata");
  const auto key = m.id.to_string();
  if (directories_.contains(key))
    return failure(StorageErrorCode::DuplicateId, m.relative_path, "Directory UUID already exists");
  for (const auto& [directory_key, d] : directories_)
    if (d.relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  if (entry_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  directory_members_.try_emplace(key);
  try {
    directories_.emplace(key, std::move(m));
  } catch (...) {
    directory_members_.erase(key);
    throw;
  }
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::update_directory(DirectoryMetadata m) {
  if (!valid_kind(m.kind) || !valid_path(m.relative_path) || !valid_utf8(m.name))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Invalid Directory metadata");
  const auto key = m.id.to_string();
  auto it = directories_.find(key);
  if (it == directories_.end())
    return failure(StorageErrorCode::NotFound, m.relative_path, "Directory UUID not found");
  if (it->second.kind != m.kind && directory_referenced(it->second))
    return failure(StorageErrorCode::ReferencedDirectory, m.relative_path,
                   "Directory is referenced by entries");
  for (const auto& [directory_key, d] : directories_)
    if (directory_key != key && d.relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  if (entry_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  static_assert(std::is_nothrow_swappable_v<DirectoryMetadata>);
  std::swap(it->second, m);
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::remove_directory(const Id& id) {
  const auto key = id.to_string();
  auto it = directories_.find(key);
  if (it == directories_.end()) return failure(StorageErrorCode::NotFound, {}, "Directory UUID not found");
  if (directory_referenced(it->second))
    return failure(StorageErrorCode::ReferencedDirectory, it->second.relative_path,
                   "Directory is referenced by entries");
  directory_members_.erase(key);
  directories_.erase(it);
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::internal::Result<std::monostate, StorageError> MetadataCatalog::rebuild(
    std::vector<LogMetadata> logs,
    std::vector<MemoMetadata> memos,
    std::vector<TodoMetadata> todos,
    std::vector<DirectoryMetadata> directories) {
  MetadataCatalog candidate;
  for (auto& directory : directories) {
    auto inserted = candidate.insert_directory(std::move(directory));
    if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
  }
  auto insert_records = [&](auto& records) -> ::folio::core::internal::Result<std::monostate, StorageError> {
    for (auto& record : records) {
      auto inserted = candidate.insert_entry(std::move(record));
      if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
    }
    return std::monostate{};
  };
  auto inserted = insert_records(logs);
  if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
  inserted = insert_records(memos);
  if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
  inserted = insert_records(todos);
  if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
  candidate.unpersisted_ = true;
  static_assert(std::is_nothrow_swappable_v<MetadataCatalog>);
  std::swap(*this, candidate);
  return std::monostate{};
}

std::optional<EntryMetadata> MetadataCatalog::find_entry(const Id& id) const {
  const auto key = id.to_string();
  if (const auto it = logs_.find(key); it != logs_.end()) return EntryMetadata{it->second};
  if (const auto it = memos_.find(key); it != memos_.end()) return EntryMetadata{it->second};
  if (const auto it = todos_.find(key); it != todos_.end()) return EntryMetadata{it->second};
  return std::nullopt;
}
std::optional<DirectoryMetadata> MetadataCatalog::find_directory(const Id& id) const {
  const auto it = directories_.find(id.to_string());
  return it == directories_.end() ? std::nullopt : std::optional<DirectoryMetadata>{it->second};
}
RootListing MetadataCatalog::list_root(EntryType module) const {
  RootListing out;
  switch (module) {
    case EntryType::Log:
      for (const auto& key : log_roots_) out.entries.emplace_back(logs_.at(key));
      break;
    case EntryType::Memo:
      for (const auto& key : memo_roots_) out.entries.emplace_back(memos_.at(key));
      break;
    case EntryType::Todo:
      for (const auto& key : todo_roots_) out.entries.emplace_back(todos_.at(key));
      break;
  }
  for (const auto& [key, d] : directories_)
    if ((module == EntryType::Memo && d.kind == DirectoryKind::Memo) ||
        (module == EntryType::Todo && d.kind == DirectoryKind::Todo)) out.directories.push_back(d);
  return out;
}
::folio::core::internal::Result<std::vector<EntryMetadata>, StorageError> MetadataCatalog::list_directory(DirectoryKind kind, const Id& id) const {
  const auto dir = find_directory(id);
  if (!dir || dir->kind != kind)
    return StorageError{StorageErrorCode::NotFound, {}, "Directory UUID not found in module", {}, false};
  std::vector<EntryMetadata> out;
  const auto& members = directory_members_.at(id.to_string());
  if (kind == DirectoryKind::Memo) {
    for (const auto& key : members) out.emplace_back(memos_.at(key));
  } else if (kind == DirectoryKind::Todo) {
    for (const auto& key : members) out.emplace_back(todos_.at(key));
  }
  return out;
}
std::vector<DirectoryMetadata> MetadataCatalog::list_directories(DirectoryKind kind) const {
  std::vector<DirectoryMetadata> out;
  for (const auto& [key, d] : directories_) if (d.kind == kind) out.push_back(d);
  return out;
}
std::vector<EntryMetadata> MetadataCatalog::query(const CatalogQuery& q) const {
  std::vector<EntryMetadata> out;
  auto append_matching = [&](const auto& collection) {
    for (const auto& [key, e] : collection)
      if (matches(e, q)) out.emplace_back(e);
  };
  if (q.directory) {
    const std::set<std::string>* members = nullptr;
    std::optional<EntryType> constrained_type;
    if (*q.directory) {
      const auto d = find_directory(**q.directory);
      if (!d) return out;
      constrained_type = d->kind == DirectoryKind::Memo ? EntryType::Memo : EntryType::Todo;
      members = &directory_members_.at(d->id.to_string());
    }
    if (!constrained_type && (!q.type || *q.type == EntryType::Log)) {
      for (const auto& key : log_roots_) {
        const auto& e = logs_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
    if ((!constrained_type || *constrained_type == EntryType::Memo) &&
        (!q.type || *q.type == EntryType::Memo)) {
      const auto& ids = members ? *members : memo_roots_;
      for (const auto& key : ids) {
        const auto& e = memos_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
    if ((!constrained_type || *constrained_type == EntryType::Todo) &&
        (!q.type || *q.type == EntryType::Todo)) {
      const auto& ids = members ? *members : todo_roots_;
      for (const auto& key : ids) {
        const auto& e = todos_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
  } else {
    if (!q.type || *q.type == EntryType::Log) append_matching(logs_);
    if (!q.type || *q.type == EntryType::Memo) append_matching(memos_);
    if (!q.type || *q.type == EntryType::Todo) append_matching(todos_);
  }
  std::sort(out.begin(), out.end(), [&](const EntryMetadata& a, const EntryMetadata& b) {
    const int cmp = compare_key(a, b, q.sort);
    if (cmp == -2) return false;
    if (cmp == 2) return true;
    if (cmp != 0) return q.descending ? cmp > 0 : cmp < 0;
    return entry_id(a).to_string() < entry_id(b).to_string();
  });
  return out;
}
bool MetadataCatalog::unpersisted() const noexcept { return unpersisted_; }
void MetadataCatalog::mark_persisted() noexcept { unpersisted_ = false; }

}  // namespace folio::core::storage
