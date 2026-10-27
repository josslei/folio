#pragma once

#include "folio/core/directory.hpp"
#include "folio/core/log.hpp"
#include "folio/core/memo.hpp"
#include "folio/core/todo.hpp"
#include "folio/core/types.hpp"

#include <toml++/toml.hpp>

#include <string>
#include <string_view>
#include <optional>
#include <variant>

namespace folio::core::storage::codec {

enum class CodecErrorCode {
  MalformedToml,
  MissingField,
  InvalidField,
  UnsupportedSchemaVersion,
  MalformedFrontMatter,
  UnknownField
};

struct CodecError {
  CodecErrorCode code;
  std::string path;
  std::string message;
};

template <class T>
using Result = std::variant<T, CodecError>;

[[nodiscard]] toml::value<std::string> to_toml(const Id& id);
[[nodiscard]] toml::value<std::string> to_toml(const Tag& tag);
[[nodiscard]] toml::value<std::string> to_toml(Privacy privacy);
[[nodiscard]] toml::table to_toml(const TimestampContext& context);
[[nodiscard]] toml::table to_toml(const DateContext& context);
[[nodiscard]] toml::table to_toml(const DueDate& due_date);
[[nodiscard]] toml::table to_toml(const Todo::Snapshot& snapshot);
[[nodiscard]] toml::table to_toml(const Directory::Snapshot& snapshot);

[[nodiscard]] Result<Id> id_from_toml(const toml::node& node);
[[nodiscard]] Result<Tag> tag_from_toml(const toml::node& node);
[[nodiscard]] Result<Privacy> privacy_from_toml(const toml::node& node);
[[nodiscard]] Result<TimestampContext> timestamp_context_from_toml(
    const toml::node& node);
[[nodiscard]] Result<DateContext> date_context_from_toml(
    const toml::node& node);
[[nodiscard]] Result<DueDate> due_date_from_toml(const toml::node& node);

[[nodiscard]] Result<Todo::Snapshot> todo_snapshot_from_toml(
    const toml::table& table);
// Legacy documents lacking kind require a trusted module context. A present
// kind must agree with that context when one is supplied.
[[nodiscard]] Result<Directory::Snapshot> directory_snapshot_from_toml(
    const toml::table& table,
    std::optional<DirectoryKind> trusted_legacy_kind = std::nullopt);

[[nodiscard]] Result<std::string> serialize_toml(const toml::table& table);
[[nodiscard]] Result<toml::table> parse_toml(std::string_view text);

// Front matter uses LF framing; decoding accepts LF or CRLF framing and
// preserves every byte after the closing delimiter's line terminator.
[[nodiscard]] Result<std::string> serialize_markdown(const Log::Snapshot& snapshot);
[[nodiscard]] Result<std::string> serialize_markdown(const Memo::Snapshot& snapshot);
[[nodiscard]] Result<Log::Snapshot> log_snapshot_from_markdown(std::string_view text);
[[nodiscard]] Result<Memo::Snapshot> memo_snapshot_from_markdown(std::string_view text);

}  // namespace folio::core::storage::codec
