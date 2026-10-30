#pragma once

#include "core/internal/result.hpp"

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

[[nodiscard]] toml::value<std::string> to_toml(const Id& id);
[[nodiscard]] toml::value<std::string> to_toml(const Tag& tag);
[[nodiscard]] toml::value<std::string> to_toml(Privacy privacy);
[[nodiscard]] toml::table to_toml(const TimestampContext& context);
[[nodiscard]] toml::table to_toml(const DateContext& context);
[[nodiscard]] toml::table to_toml(const DueDate& due_date);
[[nodiscard]] toml::table to_toml(const Todo::Snapshot& snapshot);
[[nodiscard]] toml::table to_toml(const Directory::Snapshot& snapshot);

[[nodiscard]] ::folio::core::internal::Result<Id, CodecError> id_from_toml(const toml::node& node);
[[nodiscard]] ::folio::core::internal::Result<Tag, CodecError> tag_from_toml(const toml::node& node);
[[nodiscard]] ::folio::core::internal::Result<Privacy, CodecError> privacy_from_toml(const toml::node& node);
[[nodiscard]] ::folio::core::internal::Result<TimestampContext, CodecError> timestamp_context_from_toml(
    const toml::node& node);
[[nodiscard]] ::folio::core::internal::Result<DateContext, CodecError> date_context_from_toml(
    const toml::node& node);
[[nodiscard]] ::folio::core::internal::Result<DueDate, CodecError> due_date_from_toml(const toml::node& node);

[[nodiscard]] ::folio::core::internal::Result<Todo::Snapshot, CodecError> todo_snapshot_from_toml(
    const toml::table& table);
// Legacy documents lacking kind require a trusted module context. A present
// kind must agree with that context when one is supplied.
[[nodiscard]] ::folio::core::internal::Result<Directory::Snapshot, CodecError> directory_snapshot_from_toml(
    const toml::table& table,
    std::optional<DirectoryKind> trusted_legacy_kind = std::nullopt);

[[nodiscard]] ::folio::core::internal::Result<std::string, CodecError> serialize_toml(const toml::table& table);
[[nodiscard]] ::folio::core::internal::Result<toml::table, CodecError> parse_toml(std::string_view text);

// Front matter uses LF framing; decoding accepts LF or CRLF framing and
// preserves every byte after the closing delimiter's line terminator.
[[nodiscard]] ::folio::core::internal::Result<std::string, CodecError> serialize_markdown(const Log::Snapshot& snapshot);
[[nodiscard]] ::folio::core::internal::Result<std::string, CodecError> serialize_markdown(const Memo::Snapshot& snapshot);
[[nodiscard]] ::folio::core::internal::Result<Log::Snapshot, CodecError> log_snapshot_from_markdown(std::string_view text);
[[nodiscard]] ::folio::core::internal::Result<Memo::Snapshot, CodecError> memo_snapshot_from_markdown(std::string_view text);

}  // namespace folio::core::storage::codec
