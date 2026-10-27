#include "storage/codec.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace folio::core::storage::codec {
namespace {

using namespace std::chrono;

static_assert(!std::ratio_less_v<Timestamp::duration::period, std::nano>,
              "TOML timestamp encoding supports system_clock precision up to nanoseconds");

CodecError error(CodecErrorCode code, std::string path, std::string message) {
  return CodecError{code, std::move(path), std::move(message)};
}

Result<std::string> read_string(const toml::table& table, std::string_view key,
                                std::string path) {
  const auto* node = table.get(key);
  if (!node) {
    return error(CodecErrorCode::MissingField, std::move(path), "Required field is missing");
  }
  const auto value = node->value<std::string>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, std::move(path), "Expected a string");
  }
  return *value;
}

Result<std::int64_t> read_integer(const toml::table& table, std::string_view key,
                                  std::string path) {
  const auto* node = table.get(key);
  if (!node) {
    return error(CodecErrorCode::MissingField, std::move(path), "Required field is missing");
  }
  if (!node->is_integer()) {
    return error(CodecErrorCode::InvalidField, std::move(path), "Expected an integer");
  }
  const auto value = node->value<std::int64_t>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, std::move(path), "Expected an integer");
  }
  return *value;
}

Result<bool> read_boolean(const toml::table& table, std::string_view key,
                          std::string path) {
  const auto* node = table.get(key);
  if (!node) {
    return error(CodecErrorCode::MissingField, std::move(path), "Required field is missing");
  }
  const auto value = node->value<bool>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, std::move(path), "Expected a boolean");
  }
  return *value;
}

std::optional<CodecError> check_schema_version(const toml::table& table) {
  auto result = read_integer(table, "schema_version", "schema_version");
  if (const auto* failure = std::get_if<CodecError>(&result)) {
    return *failure;
  }
  if (std::get<std::int64_t>(result) != 1) {
    return error(CodecErrorCode::UnsupportedSchemaVersion, "schema_version",
                 "Only schema version 1 is supported");
  }
  return std::nullopt;
}

std::optional<int> parse_digits(std::string_view text) {
  if (text.empty()) return std::nullopt;
  for (const char character : text) {
    if (character < '0' || character > '9') return std::nullopt;
  }
  int value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<year_month_day> parse_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  const auto year = parse_digits(text.substr(0, 4));
  const auto month = parse_digits(text.substr(5, 2));
  const auto day = parse_digits(text.substr(8, 2));
  if (!year || !month || !day) {
    return std::nullopt;
  }
  const year_month_day result{std::chrono::year{*year},
                              std::chrono::month{static_cast<unsigned>(*month)},
                              std::chrono::day{static_cast<unsigned>(*day)}};
  return result.ok() ? std::optional<year_month_day>{result} : std::nullopt;
}

std::string format_date(year_month_day date) {
  if (!date.ok()) {
    throw std::invalid_argument("Cannot encode an invalid calendar date");
  }
  const int year = static_cast<int>(date.year());
  if (year < 0 || year > 9999) {
    throw std::invalid_argument("Calendar year is outside the supported ISO date range");
  }
  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << year << '-'
         << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(date.day());
  return output.str();
}

std::string format_timestamp(Timestamp timestamp) {
  const auto seconds_since_epoch = floor<seconds>(timestamp);
  const auto day_point = floor<days>(seconds_since_epoch);
  const year_month_day date{day_point};
  const int year = static_cast<int>(date.year());
  if (!date.ok() || year < 0 || year > 9999) {
    throw std::invalid_argument("Timestamp is outside the supported RFC3339 year range");
  }

  const hh_mm_ss time{seconds_since_epoch - day_point};
  const auto fraction = duration_cast<nanoseconds>(timestamp - seconds_since_epoch).count();
  std::ostringstream output;
  output << format_date(date) << 'T' << std::setfill('0') << std::setw(2)
         << time.hours().count() << ':' << std::setw(2) << time.minutes().count()
         << ':' << std::setw(2) << time.seconds().count();
  if (fraction != 0) {
    std::ostringstream fractional;
    fractional << std::setfill('0') << std::setw(9) << fraction;
    std::string digits = fractional.str();
    while (!digits.empty() && digits.back() == '0') {
      digits.pop_back();
    }
    output << '.' << digits;
  }
  output << 'Z';
  return output.str();
}

std::optional<Timestamp> parse_timestamp(std::string_view text) {
  if (text.size() < 20 || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':') {
    return std::nullopt;
  }
  const auto date = parse_date(text.substr(0, 10));
  const auto hour = parse_digits(text.substr(11, 2));
  const auto minute = parse_digits(text.substr(14, 2));
  const auto second = parse_digits(text.substr(17, 2));
  if (!date || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
      *second > 59) {
    return std::nullopt;
  }

  std::size_t fraction_end = text.size();
  if (text.back() != 'Z') {
    return std::nullopt;
  }
  --fraction_end;
  std::int64_t fraction_ns = 0;
  if (fraction_end > 19) {
    if (text[19] != '.') {
      return std::nullopt;
    }
    const auto digits = text.substr(20, fraction_end - 20);
    if (digits.empty() || digits.size() > 9) {
      return std::nullopt;
    }
    for (const char character : digits) {
      if (character < '0' || character > '9') {
        return std::nullopt;
      }
      fraction_ns = fraction_ns * 10 + (character - '0');
    }
    for (std::size_t i = digits.size(); i < 9; ++i) {
      fraction_ns *= 10;
    }
  } else if (fraction_end != 19) {
    return std::nullopt;
  }

  const auto day_count = sys_days{*date}.time_since_epoch().count();
  const std::int64_t whole_seconds = day_count * 86400 + *hour * 3600 + *minute * 60 + *second;
  constexpr std::int64_t billion = 1'000'000'000;
  constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t minimum_floor_seconds = minimum / billion - 1;
  constexpr std::int64_t minimum_fraction = billion + minimum % billion;
  if (whole_seconds > maximum / billion ||
      (whole_seconds == maximum / billion && fraction_ns > maximum % billion) ||
      whole_seconds < minimum_floor_seconds ||
      (whole_seconds == minimum_floor_seconds && fraction_ns < minimum_fraction)) {
    return std::nullopt;
  }
  const std::int64_t nanoseconds_since_epoch =
      whole_seconds == minimum_floor_seconds
          ? minimum + (fraction_ns - minimum_fraction)
          : whole_seconds * billion + fraction_ns;
  const nanoseconds encoded{nanoseconds_since_epoch};
  const auto native_duration = duration_cast<Timestamp::duration>(encoded);
  if (duration_cast<nanoseconds>(native_duration) != encoded) {
    return std::nullopt;
  }
  return Timestamp{native_duration};
}

template <class T>
const CodecError* result_error(const Result<T>& result) {
  return std::get_if<CodecError>(&result);
}

Result<std::string> field_string(const toml::table& table,
                                 std::string_view key) {
  return read_string(table, key, std::string{key});
}

void insert_location(toml::table& table,
                     const std::optional<std::string>& location) {
  if (location) {
    table.insert_or_assign("location", *location);
  }
}

Result<std::optional<std::string>> read_location(const toml::table& table) {
  const auto* node = table.get("location");
  if (!node) {
    return std::optional<std::string>{};
  }
  const auto value = node->value<std::string>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, "location", "Expected a string");
  }
  return std::optional<std::string>{*value};
}

std::optional<CodecError> check_fields(
    const toml::table& table, std::initializer_list<std::string_view> allowed,
    std::string_view prefix = {}) {
  for (const auto& [key, value] : table) {
    bool known = false;
    for (const auto name : allowed) {
      if (key.str() == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      auto path = std::string{prefix};
      if (!path.empty()) path += '.';
      path += key.str();
      return error(CodecErrorCode::UnknownField, std::move(path), "Unknown field");
    }
  }
  return std::nullopt;
}

// These stricter field checks apply only to Markdown metadata, leaving the
// existing common-value and Todo/Directory decoders unchanged.
std::optional<CodecError> check_markdown_fields(const toml::table& table,
                                              bool is_log) {
  if (const auto failure = check_schema_version(table)) return failure;
  const auto failure = is_log
      ? check_fields(table, {"schema_version", "id", "created", "modified",
                             "title", "tags", "privacy", "log_date"})
      : check_fields(table, {"schema_version", "id", "created", "modified",
                             "title", "tags", "privacy", "directory_id"});
  if (failure) return failure;
  for (const auto key : {"created", "modified", "log_date"}) {
    const auto* node = table.get(key);
    if (node && node->is_table()) {
      const auto context_failure = std::string_view{key} == "log_date"
          ? check_fields(*node->as_table(), {"date", "timezone", "location"}, key)
          : check_fields(*node->as_table(), {"timestamp", "timezone", "location"}, key);
      if (context_failure) return context_failure;
    }
  }
  return std::nullopt;
}

template <class T>
Result<T> read_field(const toml::table& table, std::string_view key,
                     Result<T> (*decode)(const toml::node&)) {
  const auto* node = table.get(key);
  if (!node) {
    return error(CodecErrorCode::MissingField, std::string{key}, "Required field is missing");
  }
  auto result = decode(*node);
  if (auto* failure = std::get_if<CodecError>(&result)) {
    failure->path = std::string{key} +
                    (failure->path.empty() ? "" : "." + failure->path);
  }
  return result;
}

Result<std::vector<Tag>> read_tags(const toml::table& table) {
  std::vector<Tag> tags;
  const auto* node = table.get("tags");
  if (!node) {
    return error(CodecErrorCode::MissingField, "tags", "Required field is missing");
  }
  const auto* array = node->as_array();
  if (!array) {
    return error(CodecErrorCode::InvalidField, "tags", "Expected an array of string tags");
  }
  for (std::size_t i = 0; i < array->size(); ++i) {
    auto result = tag_from_toml((*array)[i]);
    if (auto* failure = std::get_if<CodecError>(&result)) {
      failure->path = "tags[" + std::to_string(i) + "]";
      return *failure;
    }
    tags.push_back(std::get<Tag>(std::move(result)));
  }
  return tags;
}

struct MarkdownParts {
  std::string_view metadata;
  std::string_view body;
};

Result<MarkdownParts> split_markdown(std::string_view text) {
  std::size_t begin;
  if (text.starts_with("+++\n")) {
    begin = 4;
  } else if (text.starts_with("+++\r\n")) {
    begin = 5;
  } else {
    return error(CodecErrorCode::MalformedFrontMatter, "",
                 "Expected an opening +++ line at byte zero with LF or CRLF");
  }
  for (auto line_begin = begin; line_begin < text.size();) {
    const auto line_end = text.find('\n', line_begin);
    if (line_end == std::string_view::npos) break;
    auto line = text.substr(line_begin, line_end - line_begin);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line == "+++") {
      return MarkdownParts{text.substr(begin, line_begin - begin),
                           text.substr(line_end + 1)};
    }
    line_begin = line_end + 1;
  }
  return error(CodecErrorCode::MalformedFrontMatter, "",
               "Expected a closing +++ line with LF or CRLF");
}

struct MarkdownMetadata {
  Id id;
  TimestampContext created;
  TimestampContext modified;
  std::string title;
  std::vector<Tag> tags;
  Privacy privacy;
};

Result<MarkdownMetadata> read_markdown_metadata(const toml::table& table) {
  auto id = read_field(table, "id", id_from_toml);
  if (const auto* failure = result_error(id)) return *failure;
  auto created = read_field(table, "created", timestamp_context_from_toml);
  if (const auto* failure = result_error(created)) return *failure;
  auto modified = read_field(table, "modified", timestamp_context_from_toml);
  if (const auto* failure = result_error(modified)) return *failure;
  auto title = field_string(table, "title");
  if (const auto* failure = result_error(title)) return *failure;
  auto tags = read_tags(table);
  if (const auto* failure = result_error(tags)) return *failure;
  auto privacy = read_field(table, "privacy", privacy_from_toml);
  if (const auto* failure = result_error(privacy)) return *failure;
  return MarkdownMetadata{std::get<Id>(std::move(id)),
                          std::get<TimestampContext>(std::move(created)),
                          std::get<TimestampContext>(std::move(modified)),
                          std::get<std::string>(std::move(title)),
                          std::get<std::vector<Tag>>(std::move(tags)),
                          std::get<Privacy>(privacy)};
}

toml::table markdown_metadata(const Id& id, const TimestampContext& created,
                              const TimestampContext& modified,
                              const std::string& title, const std::vector<Tag>& tags,
                              Privacy privacy) {
  toml::table table{{"schema_version", 1}, {"id", to_toml(id)},
                    {"created", to_toml(created)}, {"modified", to_toml(modified)},
                    {"title", title}, {"privacy", to_toml(privacy)}};
  toml::array encoded_tags;
  for (const auto& tag : tags) encoded_tags.push_back(to_toml(tag));
  table.insert("tags", std::move(encoded_tags));
  return table;
}

Result<std::string> frame_markdown(const toml::table& metadata,
                                  const std::string& body) {
  // Escaped single-line strings cannot introduce a delimiter line. Disable
  // literal strings and real tabs too, so CR, LF and tabs are escaped uniformly.
  constexpr auto flags = toml::toml_formatter::default_flags &
      ~toml::format_flags::allow_multi_line_strings &
      ~toml::format_flags::allow_literal_strings &
      ~toml::format_flags::allow_real_tabs_in_strings;
  std::ostringstream output;
  output << "+++\n" << toml::toml_formatter{metadata, flags};
  if (!output) {
    return error(CodecErrorCode::InvalidField, "", "Could not serialize Markdown metadata");
  }
  auto text = output.str();
  if (text.back() != '\n') text += '\n';
  text += "+++\n";
  text += body;
  return text;
}

}  // namespace

toml::value<std::string> to_toml(const Id& id) { return toml::value<std::string>{id.to_string()}; }

toml::value<std::string> to_toml(const Tag& tag) { return toml::value<std::string>{tag.value}; }

toml::value<std::string> to_toml(Privacy privacy) {
  switch (privacy) {
    case Privacy::Normal:
      return toml::value<std::string>{"Normal"};
    case Privacy::Protected:
      return toml::value<std::string>{"Protected"};
    case Privacy::Hidden:
      return toml::value<std::string>{"Hidden"};
  }
  throw std::invalid_argument("Cannot encode an invalid Privacy value");
}

toml::table to_toml(const TimestampContext& context) {
  if (context.timezone.empty()) {
    throw std::invalid_argument("TimestampContext timezone cannot be empty");
  }
  toml::table table{{"timestamp", format_timestamp(context.timestamp)},
                    {"timezone", context.timezone}};
  insert_location(table, context.location);
  return table;
}

toml::table to_toml(const DateContext& context) {
  if (context.timezone.empty()) {
    throw std::invalid_argument("DateContext timezone cannot be empty");
  }
  toml::table table{{"date", format_date(context.date)},
                    {"timezone", context.timezone}};
  insert_location(table, context.location);
  return table;
}

toml::table to_toml(const DueDate& due_date) {
  if (const auto* date = std::get_if<DateContext>(&due_date)) {
    auto table = to_toml(*date);
    table.insert_or_assign("kind", "Date");
    return table;
  }

  auto table = to_toml(std::get<TimestampContext>(due_date));
  table.insert_or_assign("kind", "Datetime");
  return table;
}

toml::table to_toml(const Todo::Snapshot& snapshot) {
  toml::table table{{"schema_version", 1},
                    {"id", snapshot.id.to_string()},
                    {"title", snapshot.title},
                    {"notes", snapshot.notes}};
  switch (snapshot.status) {
    case TodoStatus::Pending:
      table.insert_or_assign("status", "Pending");
      break;
    case TodoStatus::Completed:
      table.insert_or_assign("status", "Completed");
      break;
    default:
      throw std::invalid_argument("Cannot encode an invalid Todo status");
  }
  switch (snapshot.priority) {
    case TodoPriority::None:
      table.insert_or_assign("priority", "None");
      break;
    case TodoPriority::Low:
      table.insert_or_assign("priority", "Low");
      break;
    case TodoPriority::Medium:
      table.insert_or_assign("priority", "Medium");
      break;
    case TodoPriority::High:
      table.insert_or_assign("priority", "High");
      break;
    default:
      throw std::invalid_argument("Cannot encode an invalid Todo priority");
  }
  table.insert_or_assign("privacy", std::string{to_toml(snapshot.privacy).get()});
  table.insert_or_assign("created", to_toml(snapshot.created));
  table.insert_or_assign("modified", to_toml(snapshot.modified));
  if (snapshot.due_date) {
    table.insert_or_assign("due", to_toml(*snapshot.due_date));
  }
  if (snapshot.directory_id) {
    table.insert_or_assign("directory_id", snapshot.directory_id->to_string());
  }
  if (!snapshot.subtasks.empty()) {
    toml::array subtasks;
    for (const auto& subtask : snapshot.subtasks) {
      subtasks.push_back(toml::table{{"content", subtask.content},
                                    {"completed", subtask.completed}});
    }
    table.insert_or_assign("subtasks", std::move(subtasks));
  }
  return table;
}

toml::table to_toml(const Directory::Snapshot& snapshot) {
  const char* kind;
  switch (snapshot.kind) {
    case DirectoryKind::Memo: kind = "Memo"; break;
    case DirectoryKind::Todo: kind = "Todo"; break;
    default: throw std::invalid_argument("Cannot encode an invalid Directory kind");
  }
  return toml::table{{"schema_version", 1},
                     {"id", snapshot.id.to_string()},
                     {"kind", kind},
                     {"name", snapshot.name}};
}

Result<Id> id_from_toml(const toml::node& node) {
  const auto value = node.value<std::string>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, "", "Expected a UUID string");
  }
  const auto id = Id::parse(*value);
  if (!id) {
    return error(CodecErrorCode::InvalidField, "", "Expected a canonical UUIDv4 string");
  }
  return *id;
}

Result<Tag> tag_from_toml(const toml::node& node) {
  const auto value = node.value<std::string>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, "", "Expected a string tag");
  }
  return Tag{*value};
}

Result<Privacy> privacy_from_toml(const toml::node& node) {
  const auto value = node.value<std::string>();
  if (!value) {
    return error(CodecErrorCode::InvalidField, "", "Expected a Privacy string");
  }
  if (*value == "Normal") return Privacy::Normal;
  if (*value == "Protected") return Privacy::Protected;
  if (*value == "Hidden") return Privacy::Hidden;
  return error(CodecErrorCode::InvalidField, "", "Unknown Privacy value");
}

Result<TimestampContext> timestamp_context_from_toml(const toml::node& node) {
  const auto* table = node.as_table();
  if (!table) {
    return error(CodecErrorCode::InvalidField, "", "Expected a timestamp context table");
  }
  auto timestamp_result = read_string(*table, "timestamp", "timestamp");
  if (const auto* failure = result_error(timestamp_result)) return *failure;
  const auto timestamp = parse_timestamp(std::get<std::string>(timestamp_result));
  if (!timestamp) {
    return error(CodecErrorCode::InvalidField, "timestamp", "Expected a UTC RFC3339 timestamp");
  }
  auto timezone_result = field_string(*table, "timezone");
  if (const auto* failure = result_error(timezone_result)) return *failure;
  if (std::get<std::string>(timezone_result).empty()) {
    return error(CodecErrorCode::InvalidField, "timezone",
                 "Timezone identifier cannot be empty");
  }
  auto location_result = read_location(*table);
  if (const auto* failure = result_error(location_result)) return *failure;
  return TimestampContext{*timestamp, std::get<std::string>(timezone_result),
                          std::get<std::optional<std::string>>(location_result)};
}

Result<DateContext> date_context_from_toml(const toml::node& node) {
  const auto* table = node.as_table();
  if (!table) {
    return error(CodecErrorCode::InvalidField, "", "Expected a date context table");
  }
  auto date_result = read_string(*table, "date", "date");
  if (const auto* failure = result_error(date_result)) return *failure;
  const auto date = parse_date(std::get<std::string>(date_result));
  if (!date) {
    return error(CodecErrorCode::InvalidField, "date", "Expected a valid ISO calendar date");
  }
  auto timezone_result = field_string(*table, "timezone");
  if (const auto* failure = result_error(timezone_result)) return *failure;
  if (std::get<std::string>(timezone_result).empty()) {
    return error(CodecErrorCode::InvalidField, "timezone",
                 "Timezone identifier cannot be empty");
  }
  auto location_result = read_location(*table);
  if (const auto* failure = result_error(location_result)) return *failure;
  return DateContext{*date, std::get<std::string>(timezone_result),
                     std::get<std::optional<std::string>>(location_result)};
}

Result<DueDate> due_date_from_toml(const toml::node& node) {
  const auto* table = node.as_table();
  if (!table) {
    return error(CodecErrorCode::InvalidField, "due", "Expected a due table");
  }
  auto kind_result = read_string(*table, "kind", "due.kind");
  if (const auto* failure = result_error(kind_result)) return *failure;
  const auto& kind = std::get<std::string>(kind_result);
  if (kind == "Date") {
    auto result = date_context_from_toml(node);
    if (const auto* failure = result_error(result)) return *failure;
    return DueDate{std::get<DateContext>(result)};
  }
  if (kind == "Datetime") {
    auto result = timestamp_context_from_toml(node);
    if (const auto* failure = result_error(result)) return *failure;
    return DueDate{std::get<TimestampContext>(result)};
  }
  return error(CodecErrorCode::InvalidField, "due.kind", "Expected Date or Datetime");
}

Result<Todo::Snapshot> todo_snapshot_from_toml(const toml::table& table) {
  if (const auto failure = check_schema_version(table)) return *failure;

  const auto* id_node = table.get("id");
  if (!id_node) return error(CodecErrorCode::MissingField, "id", "Required field is missing");
  auto id_result = id_from_toml(*id_node);
  if (const auto* failure = result_error(id_result)) {
    auto copy = *failure;
    copy.path = "id";
    return copy;
  }

  const auto* created_node = table.get("created");
  if (!created_node) return error(CodecErrorCode::MissingField, "created", "Required field is missing");
  auto created_result = timestamp_context_from_toml(*created_node);
  if (const auto* failure = result_error(created_result)) return *failure;

  const auto* modified_node = table.get("modified");
  if (!modified_node) return error(CodecErrorCode::MissingField, "modified", "Required field is missing");
  auto modified_result = timestamp_context_from_toml(*modified_node);
  if (const auto* failure = result_error(modified_result)) return *failure;

  auto title_result = read_string(table, "title", "title");
  if (const auto* failure = result_error(title_result)) return *failure;
  auto notes_result = read_string(table, "notes", "notes");
  if (const auto* failure = result_error(notes_result)) return *failure;
  auto status_result = read_string(table, "status", "status");
  if (const auto* failure = result_error(status_result)) return *failure;
  TodoStatus status;
  if (std::get<std::string>(status_result) == "Pending") status = TodoStatus::Pending;
  else if (std::get<std::string>(status_result) == "Completed") status = TodoStatus::Completed;
  else return error(CodecErrorCode::InvalidField, "status", "Unknown Todo status");

  auto priority_result = read_string(table, "priority", "priority");
  if (const auto* failure = result_error(priority_result)) return *failure;
  TodoPriority priority;
  const auto& priority_text = std::get<std::string>(priority_result);
  if (priority_text == "None") priority = TodoPriority::None;
  else if (priority_text == "Low") priority = TodoPriority::Low;
  else if (priority_text == "Medium") priority = TodoPriority::Medium;
  else if (priority_text == "High") priority = TodoPriority::High;
  else return error(CodecErrorCode::InvalidField, "priority", "Unknown Todo priority");

  const auto* privacy_node = table.get("privacy");
  if (!privacy_node) return error(CodecErrorCode::MissingField, "privacy", "Required field is missing");
  auto privacy_result = privacy_from_toml(*privacy_node);
  if (const auto* failure = result_error(privacy_result)) {
    auto copy = *failure;
    copy.path = "privacy";
    return copy;
  }

  std::optional<DueDate> due_date;
  if (const auto* due_node = table.get("due")) {
    auto due_result = due_date_from_toml(*due_node);
    if (const auto* failure = result_error(due_result)) return *failure;
    due_date = std::get<DueDate>(std::move(due_result));
  }

  std::optional<Id> directory_id;
  if (const auto* directory_node = table.get("directory_id")) {
    auto result = id_from_toml(*directory_node);
    if (const auto* failure = result_error(result)) {
      auto copy = *failure;
      copy.path = "directory_id";
      return copy;
    }
    directory_id = std::get<Id>(std::move(result));
  }

  std::vector<Subtask> subtasks;
  if (const auto* subtasks_node = table.get("subtasks")) {
    const auto* array = subtasks_node->as_array();
    if (!array) {
      return error(CodecErrorCode::InvalidField, "subtasks", "Expected an array of tables");
    }
    std::size_t index = 0;
    for (const auto& item : *array) {
      const auto* subtask_table = item.as_table();
      const auto prefix = "subtasks[" + std::to_string(index) + "]";
      if (!subtask_table) {
        return error(CodecErrorCode::InvalidField, prefix, "Expected a subtask table");
      }
      auto content_result = read_string(*subtask_table, "content", prefix + ".content");
      if (const auto* failure = result_error(content_result)) return *failure;
      auto completed_result = read_boolean(*subtask_table, "completed", prefix + ".completed");
      if (const auto* failure = result_error(completed_result)) return *failure;
      subtasks.push_back(Subtask{std::get<std::string>(std::move(content_result)),
                                 std::get<bool>(completed_result)});
      ++index;
    }
  }

  Todo::Snapshot snapshot{std::get<Id>(std::move(id_result)),
                          std::get<TimestampContext>(std::move(created_result)),
                          std::get<TimestampContext>(std::move(modified_result)),
                          std::get<std::string>(std::move(title_result)),
                          std::get<std::string>(std::move(notes_result)),
                          status,
                          priority,
                          std::move(due_date),
                          std::move(directory_id),
                          std::get<Privacy>(std::move(privacy_result)),
                          std::move(subtasks)};
  try {
    (void)Todo::restore(snapshot);
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
  return snapshot;
}

Result<Directory::Snapshot> directory_snapshot_from_toml(
    const toml::table& table, std::optional<DirectoryKind> trusted_legacy_kind) {
  if (const auto failure = check_schema_version(table)) return *failure;
  if (trusted_legacy_kind && *trusted_legacy_kind != DirectoryKind::Memo &&
      *trusted_legacy_kind != DirectoryKind::Todo)
    return error(CodecErrorCode::InvalidField, "kind", "Invalid trusted Directory kind");
  const auto* id_node = table.get("id");
  if (!id_node) return error(CodecErrorCode::MissingField, "id", "Required field is missing");
  auto id_result = id_from_toml(*id_node);
  if (const auto* failure = result_error(id_result)) {
    auto copy = *failure;
    copy.path = "id";
    return copy;
  }
  auto name_result = read_string(table, "name", "name");
  if (const auto* failure = result_error(name_result)) return *failure;
  DirectoryKind kind;
  if (table.get("kind")) {
    auto kind_result = read_string(table, "kind", "kind");
    if (const auto* failure = result_error(kind_result)) return *failure;
    const auto& value = std::get<std::string>(kind_result);
    if (value == "Memo") kind = DirectoryKind::Memo;
    else if (value == "Todo") kind = DirectoryKind::Todo;
    else return error(CodecErrorCode::InvalidField, "kind", "Unknown Directory kind");
    if (trusted_legacy_kind && kind != *trusted_legacy_kind)
      return error(CodecErrorCode::InvalidField, "kind", "Directory kind conflicts with trusted context");
  } else if (trusted_legacy_kind) {
    kind = *trusted_legacy_kind;
  } else {
    return error(CodecErrorCode::MissingField, "kind", "Required field is missing");
  }
  return Directory::Snapshot{std::get<Id>(std::move(id_result)),
                             kind,
                             std::get<std::string>(std::move(name_result))};
}

Result<std::string> serialize_toml(const toml::table& table) {
  try {
    std::ostringstream output;
    output << table;
    if (!output) {
      return error(CodecErrorCode::InvalidField, "", "Could not serialize TOML table");
    }
    return output.str();
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
}

Result<toml::table> parse_toml(std::string_view text) {
  try {
    return toml::parse(text);
  } catch (const toml::parse_error& exception) {
    return error(CodecErrorCode::MalformedToml, "", std::string{exception.description()});
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::MalformedToml, "", exception.what());
  }
}

Result<std::string> serialize_markdown(const Log::Snapshot& snapshot) {
  try {
    (void)Log::restore(snapshot);
    auto metadata = markdown_metadata(snapshot.id, snapshot.created, snapshot.modified,
                                      snapshot.title, snapshot.tags, snapshot.privacy);
    metadata.insert("log_date", to_toml(snapshot.log_date));
    return frame_markdown(metadata, snapshot.content);
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
}

Result<std::string> serialize_markdown(const Memo::Snapshot& snapshot) {
  try {
    (void)Memo::restore(snapshot);
    auto metadata = markdown_metadata(snapshot.id, snapshot.created, snapshot.modified,
                                      snapshot.title, snapshot.tags, snapshot.privacy);
    if (snapshot.directory_id) {
      metadata.insert("directory_id", to_toml(*snapshot.directory_id));
    }
    return frame_markdown(metadata, snapshot.content);
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
}

Result<Log::Snapshot> log_snapshot_from_markdown(std::string_view text) {
  auto parts_result = split_markdown(text);
  if (const auto* failure = result_error(parts_result)) return *failure;
  const auto parts = std::get<MarkdownParts>(parts_result);
  auto table_result = parse_toml(parts.metadata);
  if (const auto* failure = result_error(table_result)) return *failure;
  const auto& table = std::get<toml::table>(table_result);
  if (const auto failure = check_markdown_fields(table, true)) return *failure;
  auto metadata_result = read_markdown_metadata(table);
  if (const auto* failure = result_error(metadata_result)) return *failure;
  auto date_result = read_field(table, "log_date", date_context_from_toml);
  if (const auto* failure = result_error(date_result)) return *failure;
  auto metadata = std::get<MarkdownMetadata>(std::move(metadata_result));
  Log::Snapshot snapshot{std::move(metadata.id), std::move(metadata.created),
                          std::move(metadata.modified), std::move(metadata.title),
                          std::string{parts.body},
                          std::get<DateContext>(std::move(date_result)),
                          std::move(metadata.tags), metadata.privacy};
  try {
    (void)Log::restore(snapshot);
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
  return snapshot;
}

Result<Memo::Snapshot> memo_snapshot_from_markdown(std::string_view text) {
  auto parts_result = split_markdown(text);
  if (const auto* failure = result_error(parts_result)) return *failure;
  const auto parts = std::get<MarkdownParts>(parts_result);
  auto table_result = parse_toml(parts.metadata);
  if (const auto* failure = result_error(table_result)) return *failure;
  const auto& table = std::get<toml::table>(table_result);
  if (const auto failure = check_markdown_fields(table, false)) return *failure;
  auto metadata_result = read_markdown_metadata(table);
  if (const auto* failure = result_error(metadata_result)) return *failure;
  std::optional<Id> directory_id;
  if (table.contains("directory_id")) {
    auto result = read_field(table, "directory_id", id_from_toml);
    if (const auto* failure = result_error(result)) return *failure;
    directory_id = std::get<Id>(std::move(result));
  }
  auto metadata = std::get<MarkdownMetadata>(std::move(metadata_result));
  Memo::Snapshot snapshot{std::move(metadata.id), std::move(metadata.created),
                           std::move(metadata.modified), std::move(metadata.title),
                           std::string{parts.body}, std::move(metadata.tags),
                           std::move(directory_id), metadata.privacy};
  try {
    (void)Memo::restore(snapshot);
  } catch (const std::exception& exception) {
    return error(CodecErrorCode::InvalidField, "", exception.what());
  }
  return snapshot;
}

}  // namespace folio::core::storage::codec
