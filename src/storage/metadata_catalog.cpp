#include "storage/metadata_catalog.hpp"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace folio::core::storage {
namespace {
using Success = Result<std::monostate>;

Success failure(StorageErrorCode code, const std::filesystem::path& path,
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
DirectoryMetadata metadata_from(const Directory::Snapshot& s, DirectoryKind kind,
                                std::filesystem::path path) {
  return DirectoryMetadata{s.id, kind, std::move(path), s.name};
}

bool MetadataCatalog::entry_path_occupied(const std::filesystem::path& path,
                                          std::optional<Id> except) const {
  auto occupied = [&](const auto& collection) {
    return std::any_of(collection.begin(), collection.end(), [&](const auto& e) {
      return e.relative_path == path && (!except || e.id != *except);
    });
  };
  return occupied(logs_) || occupied(memos_) || occupied(todos_);
}

bool MetadataCatalog::directory_referenced(const DirectoryMetadata& directory) const {
  if (directory.kind == DirectoryKind::Memo)
    return std::any_of(memos_.begin(), memos_.end(), [&](const MemoMetadata& e) {
      return e.directory_id == directory.id;
    });
  if (directory.kind == DirectoryKind::Todo)
    return std::any_of(todos_.begin(), todos_.end(), [&](const TodoMetadata& e) {
      return e.directory_id == directory.id;
    });
  return false;
}

Result<std::monostate> MetadataCatalog::insert_entry(EntryMetadata m) {
  if (!valid_entry(m)) return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Invalid entry metadata");
  if (find_entry(entry_id(m))) return failure(StorageErrorCode::DuplicateId, entry_path(m), "Entry UUID already exists");
  if (entry_path_occupied(entry_path(m)))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& d : directories_)
    if (d.relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  if (const auto dir = entry_directory(m)) {
    const auto found = find_directory(*dir);
    const auto kind = entry_type(m) == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Missing or wrong-kind Directory");
  }
  std::visit([&](auto&& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, LogMetadata>) logs_.push_back(std::move(value));
    else if constexpr (std::is_same_v<T, MemoMetadata>) memos_.push_back(std::move(value));
    else todos_.push_back(std::move(value));
  }, std::move(m));
  dirty_ = true;
  return std::monostate{};
}

Result<std::monostate> MetadataCatalog::update_entry(EntryMetadata m) {
  if (!valid_entry(m)) return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Invalid entry metadata");
  const auto existing = find_entry(entry_id(m));
  if (!existing) return failure(StorageErrorCode::NotFound, entry_path(m), "Entry UUID not found");
  if (entry_type(*existing) != entry_type(m))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Entry type changed");
  if (entry_path_occupied(entry_path(m), entry_id(m)))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& d : directories_)
    if (d.relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  if (const auto dir = entry_directory(m)) {
    const auto found = find_directory(*dir);
    const auto kind = entry_type(m) == EntryType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Missing or wrong-kind Directory");
  }
  const Id id = entry_id(m);
  std::visit([&](auto&& value) {
    using T = std::decay_t<decltype(value)>;
    auto& collection = [&]() -> auto& {
      if constexpr (std::is_same_v<T, LogMetadata>) return logs_;
      else if constexpr (std::is_same_v<T, MemoMetadata>) return memos_;
      else return todos_;
    }();
    auto it = std::find_if(collection.begin(), collection.end(), [&](const T& e) { return e.id == id; });
    static_assert(std::is_nothrow_swappable_v<T>);
    std::swap(*it, value);
  }, m);
  dirty_ = true;
  return std::monostate{};
}

Result<std::monostate> MetadataCatalog::remove_entry(const Id& id) {
  auto remove = [&](auto& collection) {
    using T = typename std::decay_t<decltype(collection)>::value_type;
    auto it = std::find_if(collection.begin(), collection.end(), [&](const T& e) { return e.id == id; });
    if (it == collection.end()) return false;
    static_assert(std::is_nothrow_move_assignable_v<T>);
    collection.erase(it);
    return true;
  };
  if (!remove(logs_) && !remove(memos_) && !remove(todos_))
    return failure(StorageErrorCode::NotFound, {}, "Entry UUID not found");
  dirty_ = true;
  return std::monostate{};
}

Result<std::monostate> MetadataCatalog::insert_directory(DirectoryMetadata m) {
  if (!valid_kind(m.kind) || !valid_path(m.relative_path) || !valid_utf8(m.name))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Invalid Directory metadata");
  if (find_directory(m.id))
    return failure(StorageErrorCode::DuplicateId, m.relative_path, "Directory UUID already exists");
  for (const auto& d : directories_)
    if (d.relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  if (entry_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  directories_.push_back(std::move(m));
  dirty_ = true;
  return std::monostate{};
}

Result<std::monostate> MetadataCatalog::update_directory(DirectoryMetadata m) {
  if (!valid_kind(m.kind) || !valid_path(m.relative_path) || !valid_utf8(m.name))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Invalid Directory metadata");
  auto it = std::find_if(directories_.begin(), directories_.end(),
                         [&](const DirectoryMetadata& d) { return d.id == m.id; });
  if (it == directories_.end())
    return failure(StorageErrorCode::NotFound, m.relative_path, "Directory UUID not found");
  if (it->kind != m.kind && directory_referenced(*it))
    return failure(StorageErrorCode::ReferencedDirectory, m.relative_path,
                   "Directory is referenced by entries");
  for (const auto& d : directories_)
    if (&d != &*it && d.relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  if (entry_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  static_assert(std::is_nothrow_swappable_v<DirectoryMetadata>);
  std::swap(*it, m);
  dirty_ = true;
  return std::monostate{};
}

Result<std::monostate> MetadataCatalog::remove_directory(const Id& id) {
  auto it = std::find_if(directories_.begin(), directories_.end(),
                         [&](const DirectoryMetadata& d) { return d.id == id; });
  if (it == directories_.end()) return failure(StorageErrorCode::NotFound, {}, "Directory UUID not found");
  if (directory_referenced(*it))
    return failure(StorageErrorCode::ReferencedDirectory, it->relative_path,
                   "Directory is referenced by entries");
  static_assert(std::is_nothrow_move_assignable_v<DirectoryMetadata>);
  directories_.erase(it);
  dirty_ = true;
  return std::monostate{};
}

std::optional<EntryMetadata> MetadataCatalog::find_entry(const Id& id) const {
  auto find = [&](const auto& collection) -> std::optional<EntryMetadata> {
    const auto it = std::find_if(collection.begin(), collection.end(),
                                 [&](const auto& e) { return e.id == id; });
    if (it == collection.end()) return std::nullopt;
    return EntryMetadata{*it};
  };
  if (auto found = find(logs_)) return found;
  if (auto found = find(memos_)) return found;
  return find(todos_);
}
std::optional<DirectoryMetadata> MetadataCatalog::find_directory(const Id& id) const {
  auto it = std::find_if(directories_.begin(), directories_.end(),
                         [&](const DirectoryMetadata& d) { return d.id == id; });
  return it == directories_.end() ? std::nullopt : std::optional<DirectoryMetadata>{*it};
}
RootListing MetadataCatalog::list_root(EntryType module) const {
  RootListing out;
  switch (module) {
    case EntryType::Log:
      for (const auto& e : logs_) out.entries.emplace_back(e);
      break;
    case EntryType::Memo:
      for (const auto& e : memos_) if (!e.directory_id) out.entries.emplace_back(e);
      break;
    case EntryType::Todo:
      for (const auto& e : todos_) if (!e.directory_id) out.entries.emplace_back(e);
      break;
  }
  for (const auto& d : directories_)
    if ((module == EntryType::Memo && d.kind == DirectoryKind::Memo) ||
        (module == EntryType::Todo && d.kind == DirectoryKind::Todo)) out.directories.push_back(d);
  std::sort(out.entries.begin(), out.entries.end(), [](const auto& a, const auto& b) {
    return entry_id(a).to_string() < entry_id(b).to_string();
  });
  std::sort(out.directories.begin(), out.directories.end(), [](const auto& a, const auto& b) {
    return a.id.to_string() < b.id.to_string();
  });
  return out;
}
Result<std::vector<EntryMetadata>> MetadataCatalog::list_directory(DirectoryKind kind, const Id& id) const {
  const auto dir = find_directory(id);
  if (!dir || dir->kind != kind)
    return StorageError{StorageErrorCode::NotFound, {}, "Directory UUID not found in module", {}, false};
  std::vector<EntryMetadata> out;
  if (kind == DirectoryKind::Memo) {
    for (const auto& e : memos_) if (e.directory_id == id) out.emplace_back(e);
  } else if (kind == DirectoryKind::Todo) {
    for (const auto& e : todos_) if (e.directory_id == id) out.emplace_back(e);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return entry_id(a).to_string() < entry_id(b).to_string();
  });
  return out;
}
std::vector<DirectoryMetadata> MetadataCatalog::list_directories(DirectoryKind kind) const {
  std::vector<DirectoryMetadata> out;
  for (const auto& d : directories_) if (d.kind == kind) out.push_back(d);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.id.to_string() < b.id.to_string();
  });
  return out;
}
std::vector<EntryMetadata> MetadataCatalog::query(const CatalogQuery& q) const {
  std::vector<EntryMetadata> out;
  auto append_matching = [&](const auto& collection) {
    for (const auto& e : collection) {
      if (matches(e, q)) out.emplace_back(e);
    }
  };
  if (!q.type || *q.type == EntryType::Log) append_matching(logs_);
  if (!q.type || *q.type == EntryType::Memo) append_matching(memos_);
  if (!q.type || *q.type == EntryType::Todo) append_matching(todos_);
  std::sort(out.begin(), out.end(), [&](const EntryMetadata& a, const EntryMetadata& b) {
    const int cmp = compare_key(a, b, q.sort);
    if (cmp == -2) return false;
    if (cmp == 2) return true;
    if (cmp != 0) return q.descending ? cmp > 0 : cmp < 0;
    return entry_id(a).to_string() < entry_id(b).to_string();
  });
  return out;
}
bool MetadataCatalog::dirty() const noexcept { return dirty_; }
void MetadataCatalog::mark_clean() noexcept { dirty_ = false; }

}  // namespace folio::core::storage
