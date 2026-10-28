#include "storage/index_codec.hpp"

#include <msgpack.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace folio::core::storage::index_codec {
namespace {
constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxRecords = 100000;
constexpr std::size_t kMaxArray = 100000;
constexpr std::size_t kMaxMap = 64;
constexpr std::size_t kMaxDepth = 16;
constexpr std::size_t kMaxNodes = 8000000;

struct Failure : std::exception {
  IndexCodecError error;
  explicit Failure(IndexCodecErrorCode code, std::string path, std::string message)
      : error{code, std::move(path), std::move(message)} {}
};
[[noreturn]] void fail(IndexCodecErrorCode code, std::string path, std::string message) {
  throw Failure(code, std::move(path), std::move(message));
}
void require(bool condition, IndexCodecErrorCode code, std::string path, std::string message) {
  if (!condition) fail(code, std::move(path), std::move(message));
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

struct BoundedBuffer {
  std::string bytes;
  void write(const char* data, std::size_t size) {
    require(size <= kMaxBytes - bytes.size(), IndexCodecErrorCode::LimitExceeded,
            "", "Index exceeds 64 MiB");
    bytes.append(data, size);
  }
};
using Packer = msgpack::packer<BoundedBuffer>;
void pack_string(Packer& packer, std::string_view text) {
  require(text.size() <= kMaxBytes, IndexCodecErrorCode::LimitExceeded, "", "String is too large");
  require(valid_utf8(text), IndexCodecErrorCode::InvalidMetadata, "", "Invalid UTF-8 string");
  packer.pack_str(static_cast<std::uint32_t>(text.size()));
  packer.pack_str_body(text.data(), static_cast<std::uint32_t>(text.size()));
}
void pack_key(Packer& packer, std::string_view key) { pack_string(packer, key); }
std::string path_text(const std::filesystem::path& path) {
  const auto bytes = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
bool safe_path(std::string_view text) {
  if (text.empty() || text.find('\0') != std::string_view::npos ||
      text.find('\\') != std::string_view::npos || text.front() == '/' ||
      !valid_utf8(text)) return false;
  const auto slash = text.find('/');
  if (text.substr(0, slash).find(':') != std::string_view::npos) return false;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('/', begin);
    const auto part = text.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    begin = end + 1;
  }
  return false;
}
void pack_path(Packer& packer, const std::filesystem::path& path) {
  const auto value = path_text(path);
  require(safe_path(value), IndexCodecErrorCode::InvalidMetadata,
          "relative_path", "Unsafe relative locator");
  pack_string(packer, value);
}

std::string privacy_text(Privacy value) {
  switch (value) {
    case Privacy::Normal: return "Normal";
    case Privacy::Protected: return "Protected";
    case Privacy::Hidden: return "Hidden";
  }
  fail(IndexCodecErrorCode::InvalidMetadata, "privacy", "Invalid Privacy");
}
std::string kind_text(DirectoryKind value) {
  switch (value) {
    case DirectoryKind::Memo: return "Memo";
    case DirectoryKind::Todo: return "Todo";
  }
  fail(IndexCodecErrorCode::InvalidMetadata, "kind", "Invalid Directory kind");
}
std::string status_text(TodoStatus value) {
  switch (value) {
    case TodoStatus::Pending: return "Pending";
    case TodoStatus::Completed: return "Completed";
  }
  fail(IndexCodecErrorCode::InvalidMetadata, "status", "Invalid Todo status");
}
std::string priority_text(TodoPriority value) {
  switch (value) {
    case TodoPriority::None: return "None";
    case TodoPriority::Low: return "Low";
    case TodoPriority::Medium: return "Medium";
    case TodoPriority::High: return "High";
  }
  fail(IndexCodecErrorCode::InvalidMetadata, "priority", "Invalid Todo priority");
}

std::string date_text(std::chrono::year_month_day value) {
  require(value.ok(), IndexCodecErrorCode::InvalidMetadata, "date", "Invalid calendar date");
  const int year = static_cast<int>(value.year());
  require(year >= 0 && year <= 9999, IndexCodecErrorCode::InvalidMetadata,
          "date", "Calendar year outside ISO range");
  char out[10];
  auto digit = [](int value) { return static_cast<char>('0' + value); };
  out[0] = digit(year / 1000); out[1] = digit((year / 100) % 10);
  out[2] = digit((year / 10) % 10); out[3] = digit(year % 10); out[4] = '-';
  const int month = static_cast<unsigned>(value.month());
  out[5] = digit(month / 10); out[6] = digit(month % 10); out[7] = '-';
  const int day = static_cast<unsigned>(value.day());
  out[8] = digit(day / 10); out[9] = digit(day % 10);
  return {out, sizeof out};
}

void pack_timestamp(Packer& p, const TimestampContext& value) {
  using namespace std::chrono;
  using Duration = Timestamp::duration;
  constexpr auto ticks_per_second = duration_cast<Duration>(seconds{1}).count();
  static_assert(ticks_per_second > 0 && 1000000000 % ticks_per_second == 0);
  const auto ticks = value.timestamp.time_since_epoch().count();
  auto seconds_value = ticks / ticks_per_second;
  auto remainder = ticks % ticks_per_second;
  if (remainder < 0) { --seconds_value; remainder += ticks_per_second; }
  const auto nanos = remainder * (1000000000 / ticks_per_second);
  require(nanos >= 0 && nanos < 1000000000, IndexCodecErrorCode::InvalidMetadata,
          "timestamp", "Invalid timestamp fraction");
  p.pack_map(value.location ? 4 : 3);
  if (value.location) { pack_key(p, "location"); pack_string(p, *value.location); }
  pack_key(p, "nanoseconds"); p.pack_uint32(static_cast<std::uint32_t>(nanos));
  pack_key(p, "seconds"); p.pack_int64(seconds_value);
  pack_key(p, "timezone"); pack_string(p, value.timezone);
}
void pack_date(Packer& p, const DateContext& value) {
  p.pack_map(value.location ? 3 : 2);
  pack_key(p, "date"); pack_string(p, date_text(value.date));
  if (value.location) { pack_key(p, "location"); pack_string(p, *value.location); }
  pack_key(p, "timezone"); pack_string(p, value.timezone);
}
void pack_due(Packer& p, const DueDate& due) {
  p.pack_map(2);
  pack_key(p, "kind"); pack_string(p, std::holds_alternative<DateContext>(due) ? "Date" : "Datetime");
  pack_key(p, "value");
  if (const auto* date = std::get_if<DateContext>(&due)) pack_date(p, *date);
  else pack_timestamp(p, std::get<TimestampContext>(due));
}
void pack_tags(Packer& p, const std::vector<Tag>& tags) {
  require(tags.size() <= kMaxArray, IndexCodecErrorCode::LimitExceeded, "tags", "Too many tags");
  p.pack_array(static_cast<std::uint32_t>(tags.size()));
  for (const auto& tag : tags) pack_string(p, tag.value);
}
void pack_directory(Packer& p, const DirectoryMetadata& d) {
  p.pack_map(4);
  pack_key(p, "id"); pack_string(p, d.id.to_string());
  pack_key(p, "kind"); pack_string(p, kind_text(d.kind));
  pack_key(p, "name"); pack_string(p, d.name);
  pack_key(p, "relative_path"); pack_path(p, d.relative_path);
}
void pack_entry(Packer& p, const EntryMetadata& e) {
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    constexpr bool log = std::is_same_v<T, LogMetadata>;
    constexpr bool memo = std::is_same_v<T, MemoMetadata>;
    if constexpr (log) p.pack_map(9);
    else if constexpr (memo) p.pack_map(value.directory_id ? 9 : 8);
    else p.pack_map(9 + (value.directory_id ? 1 : 0) + (value.due_date ? 1 : 0));
    pack_key(p, "created"); pack_timestamp(p, value.created);
    if constexpr (!log) if (value.directory_id) {
      pack_key(p, "directory_id");
      pack_string(p, value.directory_id->to_string());
    }
    if constexpr (!log && !memo) {
      if (value.due_date) { pack_key(p, "due_date"); pack_due(p, *value.due_date); }
    }
    pack_key(p, "id"); pack_string(p, value.id.to_string());
    if constexpr (log) {
      pack_key(p, "log_date"); pack_date(p, value.log_date);
    }
    pack_key(p, "modified"); pack_timestamp(p, value.modified);
    if constexpr (!log && !memo) {
      pack_key(p, "priority"); pack_string(p, priority_text(value.priority));
    }
    pack_key(p, "privacy"); pack_string(p, privacy_text(value.privacy));
    pack_key(p, "relative_path"); pack_path(p, value.relative_path);
    if constexpr (log || memo) {
      pack_key(p, "tags"); pack_tags(p, value.tags);
    } else {
      pack_key(p, "status"); pack_string(p, status_text(value.status));
    }
    pack_key(p, "title"); pack_string(p, value.title);
    pack_key(p, "type"); pack_string(p, log ? "Log" : (memo ? "Memo" : "Todo"));
  }, e);
}

// This bounded walk reads container headers before msgpack-cxx allocates its
// object tree. It also rejects trailing bytes and limits total object count.
struct Preflight {
  std::string_view bytes;
  std::size_t position = 0;
  std::size_t nodes = 0;
  std::uint64_t number(unsigned width) {
    require(width <= bytes.size() - position, IndexCodecErrorCode::Malformed,
            "", "Truncated MessagePack value");
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i)
      result = (result << 8) | static_cast<unsigned char>(bytes[position++]);
    return result;
  }
  void skip(std::uint64_t size) {
    require(size <= bytes.size() - position, IndexCodecErrorCode::Malformed,
            "", "Truncated MessagePack payload");
    position += static_cast<std::size_t>(size);
  }
  void value(std::size_t depth) {
    require(depth <= kMaxDepth, IndexCodecErrorCode::LimitExceeded, "", "Index nesting is too deep");
    require(++nodes <= kMaxNodes, IndexCodecErrorCode::LimitExceeded, "", "Too many MessagePack nodes");
    const auto code = number(1);
    std::uint64_t children = 0;
    bool map = false;
    if (code <= 0x7f || code >= 0xe0) return;
    if (code >= 0x80 && code <= 0x8f) { map = true; children = code & 0x0f; }
    else if (code >= 0x90 && code <= 0x9f) children = code & 0x0f;
    else if (code >= 0xa0 && code <= 0xbf) { skip(code & 0x1f); return; }
    else switch (code) {
      case 0xc0: case 0xc2: case 0xc3: return;
      case 0xc4: case 0xd9: skip(number(1)); return;
      case 0xc5: case 0xda: skip(number(2)); return;
      case 0xc6: case 0xdb: skip(number(4)); return;
      case 0xc7: skip(number(1) + 1); return;
      case 0xc8: skip(number(2) + 1); return;
      case 0xc9: skip(number(4) + 1); return;
      case 0xca: skip(4); return;
      case 0xcb: skip(8); return;
      case 0xcc: case 0xd0: skip(1); return;
      case 0xcd: case 0xd1: skip(2); return;
      case 0xce: case 0xd2: skip(4); return;
      case 0xcf: case 0xd3: skip(8); return;
      case 0xd4: skip(2); return;
      case 0xd5: skip(3); return;
      case 0xd6: skip(5); return;
      case 0xd7: skip(9); return;
      case 0xd8: skip(17); return;
      case 0xdc: children = number(2); break;
      case 0xdd: children = number(4); break;
      case 0xde: map = true; children = number(2); break;
      case 0xdf: map = true; children = number(4); break;
      default: fail(IndexCodecErrorCode::Malformed, "", "Invalid MessagePack marker");
    }
    require(children <= (map ? kMaxMap : kMaxArray), IndexCodecErrorCode::LimitExceeded,
            "", "MessagePack container is too large");
    if (map) children *= 2;
    for (std::uint64_t i = 0; i < children; ++i) value(depth + 1);
  }
};

using Object = msgpack::object;
const Object& as_map(const Object& object, std::string_view path) {
  require(object.type == msgpack::type::MAP, IndexCodecErrorCode::Malformed,
          std::string(path), "Expected named map");
  return object;
}
const Object& field(const Object& object, std::string_view name, std::string_view path) {
  as_map(object, path);
  for (std::uint32_t i = 0; i < object.via.map.size; ++i) {
    const auto& pair = object.via.map.ptr[i];
    const auto& key = pair.key;
    if (key.type == msgpack::type::STR &&
        std::string_view(key.via.str.ptr, key.via.str.size) == name) return pair.val;
  }
  fail(IndexCodecErrorCode::Malformed, std::string(path) + "." + std::string(name),
       "Required field is missing");
}
const Object* optional_field(const Object& object, std::string_view name) {
  as_map(object, "");
  for (std::uint32_t i = 0; i < object.via.map.size; ++i) {
    const auto& pair = object.via.map.ptr[i];
    if (pair.key.type == msgpack::type::STR &&
        std::string_view(pair.key.via.str.ptr, pair.key.via.str.size) == name) return &pair.val;
  }
  return nullptr;
}
std::string string_of(const Object& object, std::string_view path) {
  require(object.type == msgpack::type::STR, IndexCodecErrorCode::Malformed,
          std::string(path), "Expected string");
  std::string value(object.via.str.ptr, object.via.str.size);
  require(valid_utf8(value), IndexCodecErrorCode::InvalidMetadata,
          std::string(path), "Invalid UTF-8 string");
  return value;
}
std::uint64_t unsigned_of(const Object& object, std::string_view path) {
  require(object.type == msgpack::type::POSITIVE_INTEGER, IndexCodecErrorCode::Malformed,
          std::string(path), "Expected unsigned integer");
  return object.via.u64;
}
std::int64_t integer_of(const Object& object, std::string_view path) {
  if (object.type == msgpack::type::NEGATIVE_INTEGER) return object.via.i64;
  require(object.type == msgpack::type::POSITIVE_INTEGER &&
          object.via.u64 <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
          IndexCodecErrorCode::Malformed, std::string(path), "Expected signed integer");
  return static_cast<std::int64_t>(object.via.u64);
}
const Object& as_array(const Object& object, std::string_view path) {
  require(object.type == msgpack::type::ARRAY, IndexCodecErrorCode::Malformed,
          std::string(path), "Expected array");
  return object;
}
// All maps, including unknown future fields, must have unique UTF-8 string keys.
void validate_keys(const Object& object, std::size_t depth) {
  require(depth <= kMaxDepth, IndexCodecErrorCode::LimitExceeded, "", "Index nesting is too deep");
  if (object.type == msgpack::type::MAP) {
    std::set<std::string_view> names;
    for (std::uint32_t i = 0; i < object.via.map.size; ++i) {
      const auto& pair = object.via.map.ptr[i];
      require(pair.key.type == msgpack::type::STR, IndexCodecErrorCode::Malformed,
              "", "Map key must be a string");
      std::string_view key(pair.key.via.str.ptr, pair.key.via.str.size);
      require(valid_utf8(key), IndexCodecErrorCode::InvalidMetadata, "", "Invalid UTF-8 map key");
      require(names.insert(key).second, IndexCodecErrorCode::Malformed,
              std::string(key), "Duplicate map key");
      validate_keys(pair.val, depth + 1);
    }
  } else if (object.type == msgpack::type::ARRAY) {
    for (std::uint32_t i = 0; i < object.via.array.size; ++i)
      validate_keys(object.via.array.ptr[i], depth + 1);
  }
}
Id id_of(const Object& object, std::string_view path) {
  const auto value = string_of(object, path);
  const auto id = Id::parse(value);
  require(id && id->to_string() == value, IndexCodecErrorCode::InvalidMetadata,
          std::string(path), "Expected canonical UUIDv4");
  return *id;
}
std::filesystem::path path_of(const Object& object, std::string_view path) {
  const auto value = string_of(object, path);
  require(safe_path(value), IndexCodecErrorCode::InvalidMetadata,
          std::string(path), "Unsafe relative locator");
  const auto* begin = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::chrono::year_month_day date_of(const Object& object, std::string_view path) {
  const auto text = string_of(object, path);
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  require(text.size() == 10 && text[4] == '-' && text[7] == '-' &&
          digit(text[0]) && digit(text[1]) && digit(text[2]) && digit(text[3]) &&
          digit(text[5]) && digit(text[6]) && digit(text[8]) && digit(text[9]),
          IndexCodecErrorCode::InvalidMetadata, std::string(path), "Invalid ISO calendar date");
  const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                   (text[2] - '0') * 10 + text[3] - '0';
  const int month = (text[5] - '0') * 10 + text[6] - '0';
  const int day = (text[8] - '0') * 10 + text[9] - '0';
  const auto value = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                     std::chrono::day{static_cast<unsigned>(day)};
  require(value.ok(), IndexCodecErrorCode::InvalidMetadata,
          std::string(path), "Invalid calendar date");
  return value;
}
Timestamp timestamp_of(std::int64_t seconds_value, std::uint64_t nanos, std::string_view path) {
  using namespace std::chrono;
  using Duration = Timestamp::duration;
  using Rep = Duration::rep;
  constexpr auto ticks_per_second = duration_cast<Duration>(seconds{1}).count();
  static_assert(std::is_integral_v<Rep> && ticks_per_second > 0 &&
                1000000000 % ticks_per_second == 0);
  constexpr auto nanos_per_tick = 1000000000 / ticks_per_second;
  require(nanos < 1000000000 && nanos % nanos_per_tick == 0,
          IndexCodecErrorCode::InvalidMetadata, std::string(path), "Unrepresentable timestamp fraction");
  const auto fraction = static_cast<Rep>(nanos / nanos_per_tick);
  const Rep min_ticks = Timestamp::min().time_since_epoch().count();
  const Rep max_ticks = Timestamp::max().time_since_epoch().count();
  auto split = [](Rep ticks) {
    std::int64_t whole = ticks / ticks_per_second;
    Rep remainder = ticks % ticks_per_second;
    if (remainder < 0) { --whole; remainder += ticks_per_second; }
    return std::pair{whole, remainder};
  };
  const auto [min_seconds, min_fraction] = split(min_ticks);
  const auto [max_seconds, max_fraction] = split(max_ticks);
  require(seconds_value >= min_seconds && seconds_value <= max_seconds,
          IndexCodecErrorCode::InvalidMetadata, std::string(path), "Timestamp outside native range");
  Rep ticks;
  if (seconds_value == min_seconds) {
    require(fraction >= min_fraction, IndexCodecErrorCode::InvalidMetadata,
            std::string(path), "Timestamp outside native range");
    ticks = min_ticks + (fraction - min_fraction);
  } else if (seconds_value == max_seconds) {
    require(fraction <= max_fraction, IndexCodecErrorCode::InvalidMetadata,
            std::string(path), "Timestamp outside native range");
    ticks = max_ticks - (max_fraction - fraction);
  } else {
    ticks = static_cast<Rep>(seconds_value * ticks_per_second + fraction);
  }
  return Timestamp{Duration{ticks}};
}
void reject_irrelevant(const Object& object, std::initializer_list<std::string_view> names,
                       std::string_view path);
TimestampContext timestamp_context_of(const Object& object, std::string_view path) {
  as_map(object, path);
  reject_irrelevant(object, {"date"}, path);
  const auto seconds_value = integer_of(field(object, "seconds", path), std::string(path) + ".seconds");
  const auto nanos = unsigned_of(field(object, "nanoseconds", path), std::string(path) + ".nanoseconds");
  const auto timezone = string_of(field(object, "timezone", path), std::string(path) + ".timezone");
  std::optional<std::string> location;
  if (const auto* value = optional_field(object, "location"))
    location = string_of(*value, std::string(path) + ".location");
  return {timestamp_of(seconds_value, nanos, path), timezone, location};
}
DateContext date_context_of(const Object& object, std::string_view path) {
  as_map(object, path);
  reject_irrelevant(object, {"seconds", "nanoseconds"}, path);
  const auto date = date_of(field(object, "date", path), std::string(path) + ".date");
  const auto timezone = string_of(field(object, "timezone", path), std::string(path) + ".timezone");
  std::optional<std::string> location;
  if (const auto* value = optional_field(object, "location"))
    location = string_of(*value, std::string(path) + ".location");
  return {date, timezone, location};
}
DueDate due_of(const Object& object, std::string_view path) {
  as_map(object, path);
  reject_irrelevant(object, {"date", "seconds", "nanoseconds", "timezone", "location"}, path);
  const auto kind = string_of(field(object, "kind", path), std::string(path) + ".kind");
  const auto& value = field(object, "value", path);
  if (kind == "Date") return date_context_of(value, std::string(path) + ".value");
  if (kind == "Datetime") return timestamp_context_of(value, std::string(path) + ".value");
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path) + ".kind", "Invalid DueDate kind");
}
Privacy privacy_of(const Object& object, std::string_view path) {
  const auto text = string_of(object, path);
  if (text == "Normal") return Privacy::Normal;
  if (text == "Protected") return Privacy::Protected;
  if (text == "Hidden") return Privacy::Hidden;
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path), "Invalid Privacy");
}
DirectoryKind kind_of(const Object& object, std::string_view path) {
  const auto text = string_of(object, path);
  if (text == "Memo") return DirectoryKind::Memo;
  if (text == "Todo") return DirectoryKind::Todo;
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path), "Invalid Directory kind");
}
TodoStatus status_of(const Object& object, std::string_view path) {
  const auto text = string_of(object, path);
  if (text == "Pending") return TodoStatus::Pending;
  if (text == "Completed") return TodoStatus::Completed;
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path), "Invalid Todo status");
}
TodoPriority priority_of(const Object& object, std::string_view path) {
  const auto text = string_of(object, path);
  if (text == "None") return TodoPriority::None;
  if (text == "Low") return TodoPriority::Low;
  if (text == "Medium") return TodoPriority::Medium;
  if (text == "High") return TodoPriority::High;
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path), "Invalid Todo priority");
}
std::vector<Tag> tags_of(const Object& object, std::string_view path) {
  as_array(object, path);
  std::vector<Tag> tags;
  tags.reserve(object.via.array.size);
  for (std::uint32_t i = 0; i < object.via.array.size; ++i)
    tags.push_back(Tag{string_of(object.via.array.ptr[i], std::string(path) + "[" + std::to_string(i) + "]")});
  return tags;
}
void reject_irrelevant(const Object& object, std::initializer_list<std::string_view> names,
                       std::string_view path) {
  for (auto name : names)
    if (optional_field(object, name))
      fail(IndexCodecErrorCode::InvalidMetadata, std::string(path) + "." + std::string(name),
           "Field is not valid for this Entry type");
}
DirectoryMetadata directory_of(const Object& object, std::string_view path) {
  as_map(object, path);
  return {id_of(field(object, "id", path), std::string(path) + ".id"),
          kind_of(field(object, "kind", path), std::string(path) + ".kind"),
          path_of(field(object, "relative_path", path), std::string(path) + ".relative_path"),
          string_of(field(object, "name", path), std::string(path) + ".name")};
}
EntryMetadata entry_of(const Object& object, std::string_view path) {
  as_map(object, path);
  const auto type = string_of(field(object, "type", path), std::string(path) + ".type");
  const auto id = id_of(field(object, "id", path), std::string(path) + ".id");
  const auto relative_path = path_of(field(object, "relative_path", path),
                                     std::string(path) + ".relative_path");
  const auto privacy = privacy_of(field(object, "privacy", path), std::string(path) + ".privacy");
  const auto created = timestamp_context_of(field(object, "created", path), std::string(path) + ".created");
  const auto modified = timestamp_context_of(field(object, "modified", path), std::string(path) + ".modified");
  const auto title = string_of(field(object, "title", path), std::string(path) + ".title");
  if (type == "Log") {
    reject_irrelevant(object, {"directory_id", "due_date", "priority", "status"}, path);
    return LogMetadata{id, relative_path, privacy, created, modified, title,
                       tags_of(field(object, "tags", path), std::string(path) + ".tags"),
                       date_context_of(field(object, "log_date", path), std::string(path) + ".log_date")};
  }
  if (type == "Memo") {
    reject_irrelevant(object, {"due_date", "log_date", "priority", "status"}, path);
    std::optional<Id> directory_id;
    if (const auto* value = optional_field(object, "directory_id"))
      directory_id = id_of(*value, std::string(path) + ".directory_id");
    return MemoMetadata{id, relative_path, privacy, created, modified, title,
                        tags_of(field(object, "tags", path), std::string(path) + ".tags"),
                        directory_id};
  }
  if (type == "Todo") {
    reject_irrelevant(object, {"log_date", "tags"}, path);
    std::optional<Id> directory_id;
    if (const auto* value = optional_field(object, "directory_id"))
      directory_id = id_of(*value, std::string(path) + ".directory_id");
    std::optional<DueDate> due_date;
    if (const auto* value = optional_field(object, "due_date"))
      due_date = due_of(*value, std::string(path) + ".due_date");
    return TodoMetadata{id, relative_path, privacy, created, modified, title,
                        status_of(field(object, "status", path), std::string(path) + ".status"),
                        priority_of(field(object, "priority", path), std::string(path) + ".priority"),
                        due_date, directory_id};
  }
  fail(IndexCodecErrorCode::InvalidMetadata, std::string(path) + ".type", "Invalid Entry type");
}
}  // namespace

Result<std::string> encode_index(const MetadataCatalog& catalog) {
  try {
    auto directories = catalog.list_directories(DirectoryKind::Memo);
    auto todo_directories = catalog.list_directories(DirectoryKind::Todo);
    directories.insert(directories.end(), todo_directories.begin(), todo_directories.end());
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
      return a.id.to_string() < b.id.to_string();
    });
    CatalogQuery query;
    query.sort = MetadataSort::Id;
    auto entries = catalog.query(query);
    require(directories.size() + entries.size() <= kMaxRecords,
            IndexCodecErrorCode::LimitExceeded, "", "Too many Index records");
    BoundedBuffer buffer;
    Packer packer(&buffer);
    packer.pack_map(3);
    pack_key(packer, "directories");
    packer.pack_array(static_cast<std::uint32_t>(directories.size()));
    for (const auto& directory : directories) pack_directory(packer, directory);
    pack_key(packer, "entries");
    packer.pack_array(static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) pack_entry(packer, entry);
    pack_key(packer, "schema_version"); packer.pack_uint8(1);
    Preflight preflight{buffer.bytes};
    preflight.value(1);
    require(preflight.position == buffer.bytes.size(), IndexCodecErrorCode::Malformed,
            "", "Internal MessagePack encoding error");
    return std::move(buffer.bytes);
  } catch (const Failure& failure) {
    return failure.error;
  } catch (const std::bad_alloc&) {
    return IndexCodecError{IndexCodecErrorCode::LimitExceeded, "", "Index allocation failed"};
  } catch (const std::exception& exception) {
    return IndexCodecError{IndexCodecErrorCode::InvalidMetadata, "", exception.what()};
  }
}

Result<MetadataCatalog> decode_index(std::string_view bytes) {
  try {
    require(bytes.size() <= kMaxBytes, IndexCodecErrorCode::LimitExceeded,
            "", "Index exceeds 64 MiB");
    require(!bytes.empty(), IndexCodecErrorCode::Malformed, "", "Empty Index payload");
    Preflight preflight{bytes};
    preflight.value(1);
    require(preflight.position == bytes.size(), IndexCodecErrorCode::Malformed,
            "", "Trailing MessagePack bytes");
    std::size_t offset = 0;
    const msgpack::unpack_limit limit{kMaxArray, kMaxMap, kMaxBytes,
                                      kMaxBytes, kMaxBytes, kMaxDepth};
    auto handle = msgpack::unpack(bytes.data(), bytes.size(), offset,
                                  nullptr, nullptr, limit);
    require(offset == bytes.size(), IndexCodecErrorCode::Malformed,
            "", "Trailing MessagePack bytes");
    const auto& root = as_map(handle.get(), "");
    validate_keys(root, 1);
    const auto version = unsigned_of(field(root, "schema_version", ""), "schema_version");
    require(version == 1, IndexCodecErrorCode::UnsupportedVersion,
            "schema_version", "Unsupported Index schema version");
    const auto& directories = as_array(field(root, "directories", ""), "directories");
    const auto& entries = as_array(field(root, "entries", ""), "entries");
    require(static_cast<std::size_t>(directories.via.array.size) + entries.via.array.size <= kMaxRecords,
            IndexCodecErrorCode::LimitExceeded, "", "Too many Index records");
    MetadataCatalog catalog;
    for (std::uint32_t i = 0; i < directories.via.array.size; ++i) {
      const auto path = "directories[" + std::to_string(i) + "]";
      auto result = catalog.insert_directory(directory_of(directories.via.array.ptr[i], path));
      if (const auto* problem = std::get_if<StorageError>(&result))
        fail(IndexCodecErrorCode::InvalidMetadata, path, problem->message);
    }
    for (std::uint32_t i = 0; i < entries.via.array.size; ++i) {
      const auto path = "entries[" + std::to_string(i) + "]";
      auto result = catalog.insert_entry(entry_of(entries.via.array.ptr[i], path));
      if (const auto* problem = std::get_if<StorageError>(&result))
        fail(IndexCodecErrorCode::InvalidMetadata, path, problem->message);
    }
    catalog.mark_persisted();
    return catalog;
  } catch (const Failure& failure) {
    return failure.error;
  } catch (const std::bad_alloc&) {
    return IndexCodecError{IndexCodecErrorCode::LimitExceeded, "", "Index allocation failed"};
  } catch (const msgpack::unpack_error& exception) {
    return IndexCodecError{IndexCodecErrorCode::Malformed, "", exception.what()};
  } catch (const msgpack::type_error& exception) {
    return IndexCodecError{IndexCodecErrorCode::Malformed, "", exception.what()};
  } catch (const std::exception& exception) {
    return IndexCodecError{IndexCodecErrorCode::Malformed, "", exception.what()};
  }
}

}  // namespace folio::core::storage::index_codec
