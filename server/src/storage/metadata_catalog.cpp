#include "storage/metadata_catalog.hpp"

#include <immer/map.hpp>
#include <immer/set.hpp>
#include <immer/box.hpp>
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

#if defined(IMMER_NO_THREAD_SAFETY) && IMMER_NO_THREAD_SAFETY
#error "Concurrent Catalog views require Immer's thread-safe memory policy"
#endif

namespace folio::core::storage {
struct MetadataCatalog::State {
  immer::map<std::string, immer::box<LogMetadata>> logs_;
  immer::map<std::string, immer::box<MemoMetadata>> memos_;
  immer::map<std::string, immer::box<TodoMetadata>> todos_;
  immer::map<std::string, immer::box<DirectoryMetadata>> directories_;
  immer::set<std::string> log_roots_, memo_roots_, todo_roots_;
  immer::map<std::string, immer::set<std::string>> directory_members_;
};

MetadataCatalog::MetadataCatalog() : state_(std::make_shared<const State>()) {}

namespace {
::folio::core::Result<std::monostate, StorageError>
failure(StorageErrorCode code, const std::filesystem::path& path, std::string message) {
  return StorageError{code, path, std::move(message), {}, false};
}

bool equal(const core::TimestampContext& a, const core::TimestampContext& b) {
  return a.timestamp == b.timestamp && a.timezone == b.timezone && a.location == b.location;
}
bool equal(const core::DateContext& a, const core::DateContext& b) {
  return a.date == b.date && a.timezone == b.timezone && a.location == b.location;
}
bool equal(const std::optional<core::DueDate>& a, const std::optional<core::DueDate>& b) {
  if (!a || !b) return !a && !b;
  if (a->index() != b->index()) return false;
  if (const auto* date = std::get_if<core::DateContext>(&*a))
    return equal(*date, std::get<core::DateContext>(*b));
  return equal(std::get<core::TimestampContext>(*a), std::get<core::TimestampContext>(*b));
}
template <class T> bool common_equal(const T& a, const T& b) {
  return a.metadata.id == b.metadata.id && a.relative_path == b.relative_path &&
         a.read_only == b.read_only && a.metadata.privacy == b.metadata.privacy &&
         equal(a.metadata.created, b.metadata.created) &&
         equal(a.metadata.modified, b.metadata.modified) && a.metadata.title == b.metadata.title;
}
bool equal(const RecordMetadata& a, const RecordMetadata& b) {
  if (a.index() != b.index()) return false;
  if (const auto* x = std::get_if<LogMetadata>(&a)) {
    const auto& y = std::get<LogMetadata>(b);
    return common_equal(*x, y) && x->metadata.tags == y.metadata.tags &&
           equal(x->metadata.log_date, y.metadata.log_date);
  }
  if (const auto* x = std::get_if<MemoMetadata>(&a)) {
    const auto& y = std::get<MemoMetadata>(b);
    return common_equal(*x, y) && x->metadata.tags == y.metadata.tags &&
           x->metadata.directory_id == y.metadata.directory_id;
  }
  const auto& x = std::get<TodoMetadata>(a);
  const auto& y = std::get<TodoMetadata>(b);
  return common_equal(x, y) && x.metadata.status == y.metadata.status &&
         x.metadata.priority == y.metadata.priority &&
         equal(x.metadata.due_date, y.metadata.due_date) &&
         x.metadata.directory_id == y.metadata.directory_id;
}
bool equal(const DirectoryMetadata& a, const DirectoryMetadata& b) {
  return a.metadata.id == b.metadata.id && a.metadata.kind == b.metadata.kind &&
         a.relative_path == b.relative_path && a.read_only == b.read_only &&
         a.metadata.name == b.metadata.name;
}

bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) {
      ++i;
      continue;
    }
    unsigned count = 0;
    std::uint32_t code = 0;
    if ((first & 0xe0) == 0xc0) {
      count = 2;
      code = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      count = 3;
      code = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      count = 4;
      code = first & 0x07;
    } else
      return false;
    if (i + count > text.size()) return false;
    for (unsigned j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
        (count == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
    i += count;
  }
  return true;
}

bool valid_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.native().find(std::filesystem::path::value_type{}) !=
          std::filesystem::path::string_type::npos)
    return false;
  for (const auto& component : path) {
    if (component.empty() || component == "." || component == "..") return false;
  }
  const auto bytes = path.generic_u8string();
  return valid_utf8(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool valid_privacy(Privacy p) {
  return p == Privacy::Normal || p == Privacy::Protected || p == Privacy::Hidden;
}
bool valid_kind(DirectoryKind k) {
  return k == DirectoryKind::Memo || k == DirectoryKind::Todo;
}
bool valid_status(TodoStatus s) {
  return s == TodoStatus::Pending || s == TodoStatus::Completed;
}
bool valid_priority(TodoPriority p) {
  return p == TodoPriority::None || p == TodoPriority::Low || p == TodoPriority::Medium ||
         p == TodoPriority::High;
}
bool valid_context(const TimestampContext& c) {
  return valid_utf8(c.timezone) && (!c.location || valid_utf8(*c.location));
}
bool valid_context(const DateContext& c) {
  return c.date.ok() && valid_utf8(c.timezone) && (!c.location || valid_utf8(*c.location));
}
const Id& entry_id(const RecordMetadata& e) {
  return std::visit([](const auto& value) -> const Id& { return value.metadata.id; }, e);
}
const std::filesystem::path& entry_path(const RecordMetadata& e) {
  return std::visit(
      [](const auto& value) -> const std::filesystem::path& { return value.relative_path; }, e);
}
std::optional<Id> entry_directory(const RecordMetadata& e) {
  return std::visit(
      [](const auto& value) -> std::optional<Id> {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, LogMetadata>)
          return std::nullopt;
        else
          return value.metadata.directory_id;
      },
      e);
}
template <class T> void sort_metadata_ids(std::vector<T>& values) {
  std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
    if constexpr (std::is_same_v<T, RecordMetadata>)
      return entry_id(a).to_string() < entry_id(b).to_string();
    else
      return a.metadata.id.to_string() < b.metadata.id.to_string();
  });
}

Privacy entry_privacy(const RecordMetadata& e) {
  return std::visit([](const auto& value) { return value.metadata.privacy; }, e);
}
const TimestampContext& entry_created(const RecordMetadata& e) {
  return std::visit(
      [](const auto& value) -> const TimestampContext& { return value.metadata.created; }, e);
}
const TimestampContext& entry_modified(const RecordMetadata& e) {
  return std::visit(
      [](const auto& value) -> const TimestampContext& { return value.metadata.modified; }, e);
}
const std::string& entry_title(const RecordMetadata& e) {
  return std::visit([](const auto& value) -> const std::string& { return value.metadata.title; },
                    e);
}

bool valid_entry(const RecordMetadata& e) {
  if (!valid_path(entry_path(e)) || !valid_privacy(entry_privacy(e)) ||
      !valid_context(entry_created(e)) || !valid_context(entry_modified(e)) ||
      !valid_utf8(entry_title(e)))
    return false;
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TodoMetadata>) {
          if (!valid_status(value.metadata.status) || !valid_priority(value.metadata.priority))
            return false;
          if (value.metadata.due_date &&
              !std::visit([](const auto& due) { return valid_context(due); },
                          *value.metadata.due_date))
            return false;
        } else {
          for (const auto& tag : value.metadata.tags)
            if (!valid_utf8(tag.value)) return false;
          if constexpr (std::is_same_v<T, LogMetadata>)
            if (!valid_context(value.metadata.log_date)) return false;
        }
        return true;
      },
      e);
}

bool contains_ascii_folded(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  return std::search(
             haystack.begin(), haystack.end(), needle.begin(), needle.end(), [&](char a, char b) {
               return lower(static_cast<unsigned char>(a)) == lower(static_cast<unsigned char>(b));
             }) != haystack.end();
}

template <class T> bool in_range(const T& value, const InclusiveRange<T>& range) {
  return (!range.from || !(value < *range.from)) && (!range.through || !(*range.through < value));
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
template <class T> int cmp_scalar(const T& a, const T& b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}
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
int compare_key(const RecordMetadata& a, const RecordMetadata& b, MetadataSort sort) {
  switch (sort) {
  case MetadataSort::Id:
    return cmp_string(entry_id(a).to_string(), entry_id(b).to_string());
  case MetadataSort::Type:
    return cmp_scalar(static_cast<int>(record_type(a)), static_cast<int>(record_type(b)));
  case MetadataSort::Created:
    return cmp_scalar(entry_created(a).timestamp, entry_created(b).timestamp);
  case MetadataSort::Modified:
    return cmp_scalar(entry_modified(a).timestamp, entry_modified(b).timestamp);
  case MetadataSort::Title:
    return cmp_string(entry_title(a), entry_title(b));
  case MetadataSort::Privacy:
    return cmp_scalar(static_cast<int>(entry_privacy(a)), static_cast<int>(entry_privacy(b)));
  case MetadataSort::LogDate: {
    const auto* x = std::get_if<LogMetadata>(&a);
    const auto* y = std::get_if<LogMetadata>(&b);
    if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
    return cmp_scalar(std::chrono::sys_days{x->metadata.log_date.date},
                      std::chrono::sys_days{y->metadata.log_date.date});
  }
  case MetadataSort::DueDate: {
    const auto* x = std::get_if<TodoMetadata>(&a);
    const auto* y = std::get_if<TodoMetadata>(&b);
    const bool px = x && x->metadata.due_date;
    const bool py = y && y->metadata.due_date;
    if (!px || !py) return !px && !py ? 0 : (!px ? -2 : 2);
    return cmp_due(*x->metadata.due_date, *y->metadata.due_date);
  }
  case MetadataSort::Status: {
    const auto* x = std::get_if<TodoMetadata>(&a);
    const auto* y = std::get_if<TodoMetadata>(&b);
    if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
    return cmp_scalar(static_cast<int>(x->metadata.status), static_cast<int>(y->metadata.status));
  }
  case MetadataSort::Priority: {
    const auto* x = std::get_if<TodoMetadata>(&a);
    const auto* y = std::get_if<TodoMetadata>(&b);
    if (!x || !y) return !x && !y ? 0 : (!x ? -2 : 2);
    return cmp_scalar(static_cast<int>(x->metadata.priority),
                      static_cast<int>(y->metadata.priority));
  }
  }
  return 0;
}

template <class T> bool matches(const T& e, const RecordQuery& q) {
  constexpr bool log = std::is_same_v<T, LogMetadata>;
  constexpr bool memo = std::is_same_v<T, MemoMetadata>;
  constexpr bool todo = std::is_same_v<T, TodoMetadata>;
  constexpr RecordType type = log ? RecordType::Log : (memo ? RecordType::Memo : RecordType::Todo);
  if (q.type && *q.type != type) return false;
  if (q.directory) {
    if constexpr (log) {
      if (*q.directory) return false;
    } else if (*q.directory != e.metadata.directory_id)
      return false;
  }
  if (q.privacy && *q.privacy != e.metadata.privacy) return false;
  if (!contains_ascii_folded(e.metadata.title, q.text)) return false;
  if (!in_range(e.metadata.created.timestamp, q.created) ||
      !in_range(e.metadata.modified.timestamp, q.modified))
    return false;
  if (q.tag) {
    if constexpr (todo)
      return false;
    else if (!std::any_of(e.metadata.tags.begin(), e.metadata.tags.end(), [&](const Tag& tag) {
               return contains_ascii_folded(tag.value, q.tag->value);
             }))
      return false;
  }
  if (q.log_date.from || q.log_date.through) {
    if constexpr (log) {
      if (!in_date_range(e.metadata.log_date.date, q.log_date)) return false;
    } else
      return false;
  }
  if (q.todo_status || q.todo_priority || q.has_due_date || q.due_kind || q.due_from ||
      q.due_through) {
    if constexpr (!todo)
      return false;
    else {
      if (q.todo_status && *q.todo_status != e.metadata.status) return false;
      if (q.todo_priority && *q.todo_priority != e.metadata.priority) return false;
      if (q.has_due_date && *q.has_due_date != e.metadata.due_date.has_value()) return false;
      if (q.due_kind && (!e.metadata.due_date ||
                         static_cast<std::size_t>(*q.due_kind) != e.metadata.due_date->index()))
        return false;
      if (q.due_from || q.due_through) {
        if (!e.metadata.due_date) return false;
        if (q.due_from && (q.due_from->index() != e.metadata.due_date->index() ||
                           cmp_due(*e.metadata.due_date, *q.due_from) < 0))
          return false;
        if (q.due_through && (q.due_through->index() != e.metadata.due_date->index() ||
                              cmp_due(*e.metadata.due_date, *q.due_through) > 0))
          return false;
      }
    }
  }
  return true;
}
} // namespace

RecordType record_type(const RecordMetadata& metadata) noexcept {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LogMetadata>)
          return RecordType::Log;
        else if constexpr (std::is_same_v<T, MemoMetadata>)
          return RecordType::Memo;
        else
          return RecordType::Todo;
      },
      metadata);
}

RecordMetadata metadata_from(const Log::Snapshot& s, std::filesystem::path path) {
  return LogMetadata{
      core::LogMetadata{s.id, s.privacy, s.created, s.modified, s.title, s.tags, s.log_date},
      std::move(path)};
}
RecordMetadata metadata_from(const Memo::Snapshot& s, std::filesystem::path path) {
  return MemoMetadata{
      core::MemoMetadata{s.id, s.privacy, s.created, s.modified, s.title, s.tags, s.directory_id},
      std::move(path)};
}
RecordMetadata metadata_from(const Todo::Snapshot& s, std::filesystem::path path) {
  return TodoMetadata{core::TodoMetadata{s.id, s.privacy, s.created, s.modified, s.title, s.status,
                                         s.priority, s.due_date, s.directory_id},
                      std::move(path)};
}
DirectoryMetadata metadata_from(const Directory::Snapshot& s, std::filesystem::path path) {
  return DirectoryMetadata{core::DirectoryMetadata{s.id, s.kind, s.name}, std::move(path)};
}

bool metadata_matches(const RecordMetadata& left, const RecordMetadata& right) {
  return equal(left, right);
}

core::Result<std::monostate, StorageError> validate_metadata(const RecordMetadata& metadata) {
  if (!valid_entry(metadata))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(metadata),
                   "Invalid entry metadata");
  return std::monostate{};
}
core::Result<std::monostate, StorageError> validate_metadata(const DirectoryMetadata& metadata) {
  if (!valid_kind(metadata.metadata.kind) || !valid_path(metadata.relative_path) ||
      !valid_utf8(metadata.metadata.name))
    return failure(StorageErrorCode::InvalidMetadata, metadata.relative_path,
                   "Invalid Directory metadata");
  return std::monostate{};
}

MetadataCatalogView MetadataCatalog::read_view() const {
  return MetadataCatalogView{state_};
}

MetadataCatalogView::MetadataCatalogView(std::shared_ptr<const MetadataCatalog::State> state)
    : state_(std::move(state)) {}

template <class Change>
core::Result<std::monostate, StorageError> MetadataCatalog::change(Change&& apply) {
  auto next = std::make_shared<State>(*state_);
  MetadataCatalog candidate = *this;
  candidate.state_ = next;
  candidate.unpersisted_ = false;
  auto result = apply(candidate, *next);
  if (std::holds_alternative<std::monostate>(result) && candidate.unpersisted_) {
    state_ = std::move(next);
    unpersisted_ = true;
  }
  return result;
}

core::Result<std::monostate, StorageError> MetadataCatalog::insert_record(RecordMetadata metadata) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.insert_record_candidate(next, std::move(metadata));
  });
}

core::Result<std::monostate, StorageError> MetadataCatalog::update_record(RecordMetadata metadata) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.update_record_candidate(next, std::move(metadata));
  });
}

core::Result<std::monostate, StorageError> MetadataCatalog::remove_record(const Id& id) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.remove_record_candidate(next, id);
  });
}

core::Result<std::monostate, StorageError>
MetadataCatalog::insert_directory(DirectoryMetadata metadata) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.insert_directory_candidate(next, std::move(metadata));
  });
}

core::Result<std::monostate, StorageError>
MetadataCatalog::update_directory(DirectoryMetadata metadata) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.update_directory_candidate(next, std::move(metadata));
  });
}

core::Result<std::monostate, StorageError> MetadataCatalog::remove_directory(const Id& id) {
  return change([&](MetadataCatalog& candidate, State& next) {
    return candidate.remove_directory_candidate(next, id);
  });
}

bool MetadataCatalog::record_path_occupied(const std::filesystem::path& path,
                                           std::optional<Id> except) const {
  const auto& logs_ = state().logs_;
  const auto& memos_ = state().memos_;
  const auto& todos_ = state().todos_;

  auto occupied = [&](const auto& collection) {
    return std::any_of(collection.begin(), collection.end(), [&](const auto& pair) {
      const auto& e = *pair.second;
      return e.relative_path == path && (!except || e.metadata.id != *except);
    });
  };
  return occupied(logs_) || occupied(memos_) || occupied(todos_);
}

bool MetadataCatalog::directory_referenced(const DirectoryMetadata& directory) const {
  const auto& directory_members_ = state().directory_members_;

  const auto it = directory_members_.find(directory.metadata.id.to_string());
  return it && !it->empty();
}

void MetadataCatalog::change_members(State& next, RecordType type, std::optional<Id> directory_id,
                                     const std::string& key, bool insert) {
  auto change = [&](const auto& members) {
    return insert ? members.insert(key) : members.erase(key);
  };
  if (directory_id) {
    const auto id = directory_id->to_string();
    next.directory_members_ =
        next.directory_members_.set(id, change(next.directory_members_.at(id)));
  } else {
    auto& members = type == RecordType::Log    ? next.log_roots_
                    : type == RecordType::Memo ? next.memo_roots_
                                               : next.todo_roots_;
    members = change(members);
  }
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::insert_record_candidate(State& next, RecordMetadata m) {
  auto& logs_ = next.logs_;
  auto& memos_ = next.memos_;
  auto& todos_ = next.todos_;
  auto& directories_ = next.directories_;

  if (auto checked = validate_metadata(m); std::holds_alternative<StorageError>(checked))
    return checked;
  const Id id = entry_id(m);
  const auto key = id.to_string();
  if (directories_.count(key) || read_view().find_record(id))
    return failure(StorageErrorCode::DuplicateId, entry_path(m), "Entry UUID already exists");
  if (record_path_occupied(entry_path(m)))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& [directory_key, d] : directories_)
    if (d->relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  const auto type = record_type(m);
  const auto directory_id = entry_directory(m);
  if (directory_id) {
    const auto found = read_view().find_directory(*directory_id);
    const auto kind = type == RecordType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->metadata.kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m),
                     "Missing or wrong-kind Directory");
  }
  change_members(next, type, directory_id, key, true);
  {
    std::visit(
        [&](auto&& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, LogMetadata>)
            logs_ = logs_.set(key, immer::box<T>{std::move(value)});
          else if constexpr (std::is_same_v<T, MemoMetadata>)
            memos_ = memos_.set(key, immer::box<T>{std::move(value)});
          else
            todos_ = todos_.set(key, immer::box<T>{std::move(value)});
        },
        std::move(m));
  }
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::update_record_candidate(State& next, RecordMetadata m) {
  auto& logs_ = next.logs_;
  auto& memos_ = next.memos_;
  auto& todos_ = next.todos_;
  auto& directories_ = next.directories_;

  if (auto checked = validate_metadata(m); std::holds_alternative<StorageError>(checked))
    return checked;
  const Id id = entry_id(m);
  const auto key = id.to_string();
  const auto existing = read_view().find_record(id);
  if (!existing)
    return failure(StorageErrorCode::MetadataNotFound, entry_path(m), "Entry UUID not found");
  const auto type = record_type(m);
  if (record_type(*existing) != type)
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Entry type changed");
  if (record_path_occupied(entry_path(m), id))
    return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  for (const auto& [directory_key, d] : directories_)
    if (d->relative_path == entry_path(m))
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m), "Locator already occupied");
  const auto old_directory = entry_directory(*existing);
  const auto new_directory = entry_directory(m);
  if (new_directory) {
    const auto found = read_view().find_directory(*new_directory);
    const auto kind = type == RecordType::Memo ? DirectoryKind::Memo : DirectoryKind::Todo;
    if (!found || found->metadata.kind != kind)
      return failure(StorageErrorCode::InvalidMetadata, entry_path(m),
                     "Missing or wrong-kind Directory");
  }
  if (equal(*existing, m)) return std::monostate{};
  if (new_directory != old_directory) {
    change_members(next, type, old_directory, key, false);
    change_members(next, type, new_directory, key, true);
  }
  std::visit(
      [&](auto&& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LogMetadata>)
          logs_ = logs_.set(key, immer::box<T>{std::move(value)});
        else if constexpr (std::is_same_v<T, MemoMetadata>)
          memos_ = memos_.set(key, immer::box<T>{std::move(value)});
        else
          todos_ = todos_.set(key, immer::box<T>{std::move(value)});
      },
      m);
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::remove_record_candidate(State& next, const Id& id) {
  auto& logs_ = next.logs_;
  auto& memos_ = next.memos_;
  auto& todos_ = next.todos_;

  const auto key = id.to_string();
  const auto existing = read_view().find_record(id);
  if (!existing) return failure(StorageErrorCode::MetadataNotFound, {}, "Entry UUID not found");
  const auto type = record_type(*existing);
  change_members(next, type, entry_directory(*existing), key, false);
  switch (type) {
  case RecordType::Log:
    logs_ = logs_.erase(key);
    break;
  case RecordType::Memo:
    memos_ = memos_.erase(key);
    break;
  case RecordType::Todo:
    todos_ = todos_.erase(key);
    break;
  }
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::insert_directory_candidate(State& next, DirectoryMetadata m) {
  auto& directories_ = next.directories_;
  auto& directory_members_ = next.directory_members_;

  if (auto checked = validate_metadata(m); std::holds_alternative<StorageError>(checked))
    return checked;
  const auto key = m.metadata.id.to_string();
  if (directories_.count(key) || read_view().find_record(m.metadata.id))
    return failure(StorageErrorCode::DuplicateId, m.relative_path, "Entry UUID already exists");
  for (const auto& [directory_key, d] : directories_)
    if (d->relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path,
                     "Locator already occupied");
  if (record_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  directory_members_ = directory_members_.set(key, {});
  directories_ = directories_.set(key, immer::box<DirectoryMetadata>{std::move(m)});
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::update_directory_candidate(State& next, DirectoryMetadata m) {
  auto& directories_ = next.directories_;

  if (auto checked = validate_metadata(m); std::holds_alternative<StorageError>(checked))
    return checked;
  const auto key = m.metadata.id.to_string();
  auto it = directories_.find(key);
  if (!it)
    return failure(StorageErrorCode::MetadataNotFound, m.relative_path, "Directory UUID not found");
  if ((**it).metadata.kind != m.metadata.kind && directory_referenced(**it))
    return failure(StorageErrorCode::ReferencedDirectory, m.relative_path,
                   "Directory is referenced by entries");
  for (const auto& [directory_key, d] : directories_)
    if (directory_key != key && d->relative_path == m.relative_path)
      return failure(StorageErrorCode::InvalidMetadata, m.relative_path,
                     "Locator already occupied");
  if (record_path_occupied(m.relative_path))
    return failure(StorageErrorCode::InvalidMetadata, m.relative_path, "Locator already occupied");
  if (equal(**it, m)) return std::monostate{};
  directories_ = directories_.set(key, immer::box<DirectoryMetadata>{std::move(m)});
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::remove_directory_candidate(State& next, const Id& id) {
  auto& directories_ = next.directories_;
  auto& directory_members_ = next.directory_members_;

  const auto key = id.to_string();
  auto it = directories_.find(key);
  if (!it) return failure(StorageErrorCode::MetadataNotFound, {}, "Directory UUID not found");
  if (directory_referenced(**it))
    return failure(StorageErrorCode::ReferencedDirectory, (**it).relative_path,
                   "Directory is referenced by entries");
  directory_members_ = directory_members_.erase(key);
  directories_ = directories_.erase(key);
  unpersisted_ = true;
  return std::monostate{};
}

::folio::core::Result<std::monostate, StorageError>
MetadataCatalog::rebuild(std::vector<LogMetadata> logs, std::vector<MemoMetadata> memos,
                         std::vector<TodoMetadata> todos,
                         std::vector<DirectoryMetadata> directories) {
  MetadataCatalog candidate;
  for (auto& directory : directories) {
    auto inserted = candidate.insert_directory(std::move(directory));
    if (const auto* problem = std::get_if<StorageError>(&inserted)) return *problem;
  }
  auto insert_records = [&](auto& records) -> ::folio::core::Result<std::monostate, StorageError> {
    for (auto& record : records) {
      auto inserted = candidate.insert_record(std::move(record));
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

bool MetadataCatalogView::contains_entry(const Id& id) const {
  const auto& logs_ = state_->logs_;
  const auto& memos_ = state_->memos_;
  const auto& todos_ = state_->todos_;
  const auto& directories_ = state_->directories_;

  const auto key = id.to_string();
  return logs_.count(key) || memos_.count(key) || todos_.count(key) || directories_.count(key);
}

std::optional<RecordMetadata> MetadataCatalogView::find_record(const Id& id) const {
  const auto& logs_ = state_->logs_;
  const auto& memos_ = state_->memos_;
  const auto& todos_ = state_->todos_;

  const auto key = id.to_string();
  if (const auto* it = logs_.find(key)) return RecordMetadata{**it};
  if (const auto* it = memos_.find(key)) return RecordMetadata{**it};
  if (const auto* it = todos_.find(key)) return RecordMetadata{**it};
  return std::nullopt;
}
std::optional<DirectoryMetadata> MetadataCatalogView::find_directory(const Id& id) const {
  const auto& directories_ = state_->directories_;

  const auto it = directories_.find(id.to_string());
  return !it ? std::nullopt : std::optional<DirectoryMetadata>{**it};
}
std::size_t MetadataCatalogView::record_count(RecordType type, std::optional<Id> directory) const {
  if (directory) {
    const auto* members = state_->directory_members_.find(directory->to_string());
    return members ? members->size() : 0;
  }
  switch (type) {
  case RecordType::Log:
    return state_->log_roots_.size();
  case RecordType::Memo:
    return state_->memo_roots_.size();
  case RecordType::Todo:
    return state_->todo_roots_.size();
  }
  return 0;
}
std::size_t MetadataCatalogView::directory_count(DirectoryKind kind) const {
  return std::count_if(state_->directories_.begin(), state_->directories_.end(),
                       [kind](const auto& pair) { return pair.second->metadata.kind == kind; });
}

RootListing MetadataCatalogView::list_root(RecordType module) const {
  const auto& logs_ = state_->logs_;
  const auto& memos_ = state_->memos_;
  const auto& todos_ = state_->todos_;
  const auto& directories_ = state_->directories_;
  const auto& log_roots_ = state_->log_roots_;
  const auto& memo_roots_ = state_->memo_roots_;
  const auto& todo_roots_ = state_->todo_roots_;

  RootListing out;
  switch (module) {
  case RecordType::Log:
    for (const auto& key : log_roots_)
      out.records.emplace_back(*logs_.at(key));
    break;
  case RecordType::Memo:
    for (const auto& key : memo_roots_)
      out.records.emplace_back(*memos_.at(key));
    break;
  case RecordType::Todo:
    for (const auto& key : todo_roots_)
      out.records.emplace_back(*todos_.at(key));
    break;
  }
  for (const auto& [key, d] : directories_)
    if ((module == RecordType::Memo && d->metadata.kind == DirectoryKind::Memo) ||
        (module == RecordType::Todo && d->metadata.kind == DirectoryKind::Todo))
      out.directories.push_back(*d);
  sort_metadata_ids(out.records);
  sort_metadata_ids(out.directories);
  return out;
}
::folio::core::Result<std::vector<RecordMetadata>, StorageError>
MetadataCatalogView::list_directory(DirectoryKind kind, const Id& id) const {
  const auto& memos_ = state_->memos_;
  const auto& todos_ = state_->todos_;
  const auto& directory_members_ = state_->directory_members_;

  const auto dir = find_directory(id);
  if (!dir || dir->metadata.kind != kind)
    return StorageError{
        StorageErrorCode::MetadataNotFound, {}, "Directory UUID not found in module", {}, false};
  std::vector<RecordMetadata> out;
  const auto& members = directory_members_.at(id.to_string());
  if (kind == DirectoryKind::Memo) {
    for (const auto& key : members)
      out.emplace_back(*memos_.at(key));
  } else if (kind == DirectoryKind::Todo) {
    for (const auto& key : members)
      out.emplace_back(*todos_.at(key));
  }
  sort_metadata_ids(out);
  return out;
}
std::vector<DirectoryMetadata> MetadataCatalogView::list_directories(DirectoryKind kind) const {
  const auto& directories_ = state_->directories_;

  std::vector<DirectoryMetadata> out;
  for (const auto& [key, d] : directories_)
    if (d->metadata.kind == kind) out.push_back(*d);
  sort_metadata_ids(out);
  return out;
}
std::vector<RecordMetadata> MetadataCatalogView::query(const RecordQuery& q) const {
  const auto& logs_ = state_->logs_;
  const auto& memos_ = state_->memos_;
  const auto& todos_ = state_->todos_;
  const auto& log_roots_ = state_->log_roots_;
  const auto& memo_roots_ = state_->memo_roots_;
  const auto& todo_roots_ = state_->todo_roots_;
  const auto& directory_members_ = state_->directory_members_;

  std::vector<RecordMetadata> out;
  auto append_matching = [&](const auto& collection) {
    for (const auto& [key, e] : collection)
      if (matches(*e, q)) out.emplace_back(*e);
  };
  if (q.directory) {
    const immer::set<std::string>* members = nullptr;
    std::optional<RecordType> constrained_type;
    if (*q.directory) {
      const auto d = find_directory(**q.directory);
      if (!d) return out;
      constrained_type =
          d->metadata.kind == DirectoryKind::Memo ? RecordType::Memo : RecordType::Todo;
      members = &directory_members_.at(d->metadata.id.to_string());
    }
    if (!constrained_type && (!q.type || *q.type == RecordType::Log)) {
      for (const auto& key : log_roots_) {
        const auto& e = *logs_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
    if ((!constrained_type || *constrained_type == RecordType::Memo) &&
        (!q.type || *q.type == RecordType::Memo)) {
      const auto& ids = members ? *members : memo_roots_;
      for (const auto& key : ids) {
        const auto& e = *memos_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
    if ((!constrained_type || *constrained_type == RecordType::Todo) &&
        (!q.type || *q.type == RecordType::Todo)) {
      const auto& ids = members ? *members : todo_roots_;
      for (const auto& key : ids) {
        const auto& e = *todos_.at(key);
        if (matches(e, q)) out.emplace_back(e);
      }
    }
  } else {
    if (!q.type || *q.type == RecordType::Log) append_matching(logs_);
    if (!q.type || *q.type == RecordType::Memo) append_matching(memos_);
    if (!q.type || *q.type == RecordType::Todo) append_matching(todos_);
  }
  std::sort(out.begin(), out.end(), [&](const RecordMetadata& a, const RecordMetadata& b) {
    const int cmp = compare_key(a, b, q.sort);
    if (cmp == -2) return false;
    if (cmp == 2) return true;
    if (cmp != 0) return q.descending ? cmp > 0 : cmp < 0;
    return entry_id(a).to_string() < entry_id(b).to_string();
  });
  return out;
}
bool MetadataCatalog::unpersisted() const noexcept {
  return unpersisted_;
}
void MetadataCatalog::mark_persisted() noexcept {
  unpersisted_ = false;
}

} // namespace folio::core::storage
