#include "storage/codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace {

using namespace folio::core;
namespace codec = folio::core::storage::codec;

template <typename T>
bool succeeded(const codec::Result<T>& result) {
  return std::holds_alternative<T>(result);
}

void write_sample(const std::string& filename, const std::string& text) {
  const auto path = std::filesystem::path{FOLIO_SERIALIZATION_SAMPLES_DIR} / filename;
  SCOPED_TRACE(path.string());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  ASSERT_FALSE(output.fail());

  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const std::string bytes{std::istreambuf_iterator<char>{input},
                          std::istreambuf_iterator<char>{}};
  ASSERT_FALSE(input.bad());
  EXPECT_EQ(bytes, text);
}

template <typename Model, typename Decode>
void export_markdown(const std::string& filename,
                     const typename Model::Snapshot& snapshot, Decode decode) {
  SCOPED_TRACE(filename);
  const auto encoded = codec::serialize_markdown(snapshot);
  ASSERT_TRUE(succeeded(encoded));
  const auto& text = std::get<std::string>(encoded);
  const auto decoded = decode(text);
  ASSERT_TRUE(succeeded(decoded));
  const auto restored = Model::restore(std::get<typename Model::Snapshot>(decoded));
  const auto reencoded = codec::serialize_markdown(restored.snapshot());
  ASSERT_TRUE(succeeded(reencoded));
  EXPECT_EQ(std::get<std::string>(reencoded), text);
  ASSERT_NO_FATAL_FAILURE(write_sample(filename, text));
}

template <typename Model, typename Decode>
void export_toml(const std::string& filename,
                 const typename Model::Snapshot& snapshot, Decode decode) {
  SCOPED_TRACE(filename);
  const auto encoded = codec::serialize_toml(codec::to_toml(snapshot));
  ASSERT_TRUE(succeeded(encoded));
  const auto& text = std::get<std::string>(encoded);
  const auto parsed = codec::parse_toml(text);
  ASSERT_TRUE(succeeded(parsed));
  const auto decoded = decode(std::get<toml::table>(parsed));
  ASSERT_TRUE(succeeded(decoded));
  const auto restored = Model::restore(std::get<typename Model::Snapshot>(decoded));
  const auto reencoded = codec::serialize_toml(codec::to_toml(restored.snapshot()));
  ASSERT_TRUE(succeeded(reencoded));
  EXPECT_EQ(std::get<std::string>(reencoded), text);
  ASSERT_NO_FATAL_FAILURE(write_sample(filename, text));
}

template <typename T>
void expect_common_roundtrip(const toml::table& table, const std::string& key,
                             codec::Result<T> (*decode)(const toml::node&)) {
  SCOPED_TRACE(key);
  const auto* node = table.get(key);
  ASSERT_NE(node, nullptr);
  const auto decoded = decode(*node);
  ASSERT_TRUE(succeeded(decoded));
  auto reencoded = codec::to_toml(std::get<T>(decoded));
  const auto* original = node->as<decltype(reencoded)>();
  ASSERT_NE(original, nullptr);
  EXPECT_EQ(reencoded, *original);
}

TEST(CoreCodecSamplesTest, GeneratesSerializedExamples) {
  const std::filesystem::path output_dir{FOLIO_SERIALIZATION_SAMPLES_DIR};
  ASSERT_TRUE(output_dir.is_absolute());
  std::error_code error;
  std::filesystem::create_directories(output_dir, error);
  ASSERT_FALSE(error) << error.message();

  const auto log_id = Id::parse("11111111-1111-4111-8111-111111111111");
  const auto memo_id = Id::parse("22222222-2222-4222-8222-222222222222");
  const auto todo_id = Id::parse("33333333-3333-4333-8333-333333333333");
  const auto directory_id = Id::parse("44444444-4444-4444-8444-444444444444");
  ASSERT_TRUE(log_id && memo_id && todo_id && directory_id);
  const TimestampContext created{
      Timestamp{std::chrono::seconds{1761436800} + std::chrono::milliseconds{123}},
      "Asia/Shanghai", "上海"};
  const TimestampContext modified{Timestamp{std::chrono::seconds{1761523200}},
                                   "Asia/Tokyo", "東京"};
  const DateContext date{{std::chrono::year{2025} / 10 / 20},
                          "Europe/Paris", "Paris — 図書館"};
  const DueDate due_date = date;
  const DueDate due_datetime = modified;
  const std::string body =
      "# 旅の記録\n\nSynthetic sample — café.\n\n"
      "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
      "```text\nUnicode: 你好\n```\n+++\nBody delimiter stays intact.\n";
  const Log::Snapshot log{*log_id, created, modified, "週末の記録", body, date,
                           {{"旅行"}, {"café"}}, Privacy::Protected};
  const Memo::Snapshot memo{*memo_id, created, modified, "研究メモ", body,
                             {{"ideas"}, {"読書"}}, *directory_id, Privacy::Hidden};
  ASSERT_NO_FATAL_FAILURE(export_markdown<Log>(
      "log.md", log, codec::log_snapshot_from_markdown));
  ASSERT_NO_FATAL_FAILURE(export_markdown<Memo>(
      "memo.md", memo, codec::memo_snapshot_from_markdown));

  const Todo::Snapshot todo_date{
      *todo_id, created, modified, "旅行の準備",
      "First line\n第二行 — café", TodoStatus::Pending,
      TodoPriority::High, due_date, *directory_id, Privacy::Normal,
      {{"切符を予約", true}, {"Pack café notes", false}}};
  auto todo_datetime = todo_date;
  todo_datetime.due_date = due_datetime;
  todo_datetime.status = TodoStatus::Completed;
  todo_datetime.priority = TodoPriority::Medium;
  todo_datetime.privacy = Privacy::Protected;
  const TimestampContext minimal_context{created.timestamp, "UTC", std::nullopt};
  const Todo::Snapshot minimal{
      *todo_id, minimal_context, minimal_context, "Simple task", "", TodoStatus::Pending,
      TodoPriority::None, std::nullopt, std::nullopt, Privacy::Normal, {}};
  const auto minimal_table = codec::to_toml(minimal);
  for (const auto* key : {"due", "directory_id", "subtasks"}) {
    EXPECT_FALSE(minimal_table.contains(key));
  }
  ASSERT_NO_FATAL_FAILURE(export_toml<Todo>(
      "todo-date.toml", todo_date, codec::todo_snapshot_from_toml));
  ASSERT_NO_FATAL_FAILURE(export_toml<Todo>(
      "todo-datetime.toml", todo_datetime, codec::todo_snapshot_from_toml));
  ASSERT_NO_FATAL_FAILURE(export_toml<Todo>(
      "todo-minimal.toml", minimal, codec::todo_snapshot_from_toml));
  ASSERT_NO_FATAL_FAILURE(export_toml<Directory>(
      "directory.toml", Directory::Snapshot{*directory_id, "研究資料 — café"},
      codec::directory_snapshot_from_toml));

  // A collection of embedded common values for inspection, not a storage schema.
  const toml::table common{
      {"id", codec::to_toml(*log_id)}, {"tag", codec::to_toml(Tag{"旅行"})},
      {"privacy_normal", codec::to_toml(Privacy::Normal)},
      {"privacy_protected", codec::to_toml(Privacy::Protected)},
      {"privacy_hidden", codec::to_toml(Privacy::Hidden)},
      {"timestamp_context", codec::to_toml(created)},
      {"date_context", codec::to_toml(date)},
      {"due_date", codec::to_toml(due_date)},
      {"due_datetime", codec::to_toml(due_datetime)}};
  const auto encoded = codec::serialize_toml(common);
  ASSERT_TRUE(succeeded(encoded));
  const auto& text = std::get<std::string>(encoded);
  const auto parsed = codec::parse_toml(text);
  ASSERT_TRUE(succeeded(parsed));
  const auto& table = std::get<toml::table>(parsed);
  EXPECT_EQ(table, common);
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(table, "id", codec::id_from_toml));
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(table, "tag", codec::tag_from_toml));
  for (const auto* key : {"privacy_normal", "privacy_protected", "privacy_hidden"}) {
    ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(table, key, codec::privacy_from_toml));
  }
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(
      table, "timestamp_context", codec::timestamp_context_from_toml));
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(
      table, "date_context", codec::date_context_from_toml));
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(table, "due_date", codec::due_date_from_toml));
  ASSERT_NO_FATAL_FAILURE(expect_common_roundtrip(table, "due_datetime", codec::due_date_from_toml));
  ASSERT_NO_FATAL_FAILURE(write_sample("common-values.toml", text));
}

}  // namespace
