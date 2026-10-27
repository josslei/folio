#include "storage/index_codec.hpp"

#include <msgpack.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;
namespace codec = folio::core::storage::index_codec;

core::Id id(const char* text) {
  const auto parsed = core::Id::parse(text);
  if (!parsed) throw std::runtime_error("Invalid test UUID");
  return *parsed;
}

core::Timestamp timestamp_at(std::chrono::seconds seconds,
                             std::chrono::nanoseconds fraction = {}) {
  return core::Timestamp{std::chrono::duration_cast<core::Timestamp::duration>(
      seconds + fraction)};
}

core::TimestampContext context(core::Timestamp timestamp, std::string timezone,
                               std::optional<std::string> location) {
  return {timestamp, std::move(timezone), std::move(location)};
}

template <typename T>
const T& value_of(const codec::Result<T>& result) {
  return std::get<T>(result);
}

template <typename T>
void expect_error(const codec::Result<T>& result,
                  codec::IndexCodecErrorCode code) {
  ASSERT_TRUE(std::holds_alternative<codec::IndexCodecError>(result));
  EXPECT_EQ(std::get<codec::IndexCodecError>(result).code, code);
}

// Preserve map order and duplicate keys when mutating independent wire fixtures.
struct WireValue {
  using Array = std::vector<WireValue>;
  using Map = std::vector<std::pair<std::string, WireValue>>;
  std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t,
               std::string, Array, Map> value;
};

WireValue unpack_value(const msgpack::object& object) {
  switch (object.type) {
    case msgpack::type::NIL: return {nullptr};
    case msgpack::type::BOOLEAN: return {object.via.boolean};
    case msgpack::type::POSITIVE_INTEGER: return {object.via.u64};
    case msgpack::type::NEGATIVE_INTEGER: return {object.via.i64};
    case msgpack::type::STR:
      return {std::string{object.via.str.ptr, object.via.str.size}};
    case msgpack::type::ARRAY: {
      WireValue::Array values;
      for (std::uint32_t i = 0; i < object.via.array.size; ++i)
        values.push_back(unpack_value(object.via.array.ptr[i]));
      return {std::move(values)};
    }
    case msgpack::type::MAP: {
      WireValue::Map values;
      for (std::uint32_t i = 0; i < object.via.map.size; ++i) {
        const auto& pair = object.via.map.ptr[i];
        values.emplace_back(pair.key.as<std::string>(), unpack_value(pair.val));
      }
      return {std::move(values)};
    }
    default: throw std::runtime_error("Unexpected fixture MessagePack type");
  }
}

void pack_value(msgpack::packer<msgpack::sbuffer>& packer,
                const WireValue& wire) {
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, std::nullptr_t>) packer.pack_nil();
    else if constexpr (std::is_same_v<T, bool>) packer.pack(value);
    else if constexpr (std::is_same_v<T, std::int64_t>) packer.pack_int64(value);
    else if constexpr (std::is_same_v<T, std::uint64_t>) packer.pack_uint64(value);
    else if constexpr (std::is_same_v<T, std::string>) {
      packer.pack_str(static_cast<std::uint32_t>(value.size()));
      packer.pack_str_body(value.data(), static_cast<std::uint32_t>(value.size()));
    } else if constexpr (std::is_same_v<T, WireValue::Array>) {
      packer.pack_array(static_cast<std::uint32_t>(value.size()));
      for (const auto& child : value) pack_value(packer, child);
    } else {
      packer.pack_map(static_cast<std::uint32_t>(value.size()));
      for (const auto& [key, child] : value) {
        packer.pack(key);
        pack_value(packer, child);
      }
    }
  }, wire.value);
}

std::string pack_wire(const WireValue& wire) {
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> packer{buffer};
  pack_value(packer, wire);
  return {buffer.data(), buffer.size()};
}

WireValue unpack_wire(std::string_view bytes) {
  const auto handle = msgpack::unpack(bytes.data(), bytes.size());
  return unpack_value(handle.get());
}

WireValue& field(WireValue& wire, std::string_view key) {
  for (auto& [name, value] : std::get<WireValue::Map>(wire.value))
    if (name == key) return value;
  throw std::runtime_error("Missing fixture field");
}

bool has_field(const WireValue& wire, std::string_view key) {
  for (const auto& [name, value] : std::get<WireValue::Map>(wire.value)) {
    (void)value;
    if (name == key) return true;
  }
  return false;
}

void erase_field(WireValue& wire, std::string_view key) {
  auto& fields = std::get<WireValue::Map>(wire.value);
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    if (it->first == key) {
      fields.erase(it);
      return;
    }
  }
  throw std::runtime_error("Missing fixture field");
}

WireValue& entry(WireValue& wire, std::size_t index = 0) {
  return std::get<WireValue::Array>(field(wire, "entries").value).at(index);
}

WireValue& directory(WireValue& wire, std::size_t index = 0) {
  return std::get<WireValue::Array>(field(wire, "directories").value).at(index);
}

storage::MetadataCatalog make_catalog() {
  storage::MetadataCatalog catalog;
  const auto memo_dir = id("87183823-2574-4bfd-b411-99ed177d3e43");
  const auto todo_dir = id("97183823-2574-4bfd-b411-99ed177d3e43");
  const auto memo_path = fs::path{"vault/memo"} / memo_dir.to_string();
  const auto todo_path = fs::path{"vault/todo"} / todo_dir.to_string();
  if (!std::holds_alternative<std::monostate>(catalog.insert_directory(
          {memo_dir, core::DirectoryKind::Memo, memo_path, "研究 / Café"})) ||
      !std::holds_alternative<std::monostate>(catalog.insert_directory(
          {todo_dir, core::DirectoryKind::Todo, todo_path, "任务"})))
    throw std::runtime_error("Invalid Directory fixture");

  const auto created = context(timestamp_at(std::chrono::seconds{1800000000},
                       std::chrono::nanoseconds{123456000}), "Asia/Shanghai", "上海");
  const auto modified = context(timestamp_at(std::chrono::seconds{1800000090},
                        std::chrono::nanoseconds{987654000}), "Europe/Paris", std::nullopt);
  const std::string long_title(4096, 'a');
  const storage::LogMetadata log{
      id("17183823-2574-4bfd-b411-99ed177d3e43"), "vault/log.enc",
      core::Privacy::Protected, created, modified, long_title + " 旅行",
      {{"travel"}, {"旅行"}},
      {{std::chrono::year{2027} / 1 / 15}, "Asia/Tokyo", "東京"}};
  const storage::MemoMetadata memo{
      id("27183823-2574-4bfd-b411-99ed177d3e43"), memo_path / "memo.enc",
      core::Privacy::Normal, created, modified, "Memo Café", {{"notes"}}, memo_dir};
  const storage::TodoMetadata todo_date{
      id("37183823-2574-4bfd-b411-99ed177d3e43"), todo_path / "date.enc",
      core::Privacy::Hidden, created, modified, "Todo 日付",
      core::TodoStatus::Completed, core::TodoPriority::High,
      core::DueDate{core::DateContext{{std::chrono::year{2028} / 2 / 3},
                                      "Asia/Tokyo", "東京"}}, todo_dir};
  const storage::TodoMetadata todo_time{
      id("47183823-2574-4bfd-b411-99ed177d3e43"), "vault/todo-root.enc",
      core::Privacy::Normal, created, modified, "Todo time",
      core::TodoStatus::Pending, core::TodoPriority::Low,
      core::DueDate{context(timestamp_at(std::chrono::seconds{1900000000},
                             std::chrono::nanoseconds{654321000}),
                            "UTC", std::nullopt)}, std::nullopt};
  for (const storage::EntryMetadata& metadata : {
           storage::EntryMetadata{log}, storage::EntryMetadata{memo},
           storage::EntryMetadata{todo_date}, storage::EntryMetadata{todo_time}}) {
    if (!std::holds_alternative<std::monostate>(catalog.insert_entry(metadata)))
      throw std::runtime_error("Invalid Entry fixture");
  }
  return catalog;
}

std::string encoded_catalog() {
  const auto encoded = codec::encode_index(make_catalog());
  if (!std::holds_alternative<std::string>(encoded))
    throw std::runtime_error("Could not encode fixture catalog");
  return value_of(encoded);
}

TEST(IndexCodecTest, RoundTripsAllMetadataFieldsAndRestoresMembershipClean) {
  const auto source = make_catalog();
  ASSERT_TRUE(source.dirty());
  const auto encoded = codec::encode_index(source);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  const auto decoded = codec::decode_index(value_of(encoded));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  const auto& restored = value_of(decoded);
  EXPECT_FALSE(restored.dirty());
  EXPECT_EQ(restored.query().size(), 4U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Log).entries.size(), 1U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Memo).directories.size(), 1U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Todo).entries.size(), 1U);
  EXPECT_EQ(restored.list_root(storage::EntryType::Todo).directories.size(), 1U);

  const auto memo_dir = id("87183823-2574-4bfd-b411-99ed177d3e43");
  const auto todo_dir = id("97183823-2574-4bfd-b411-99ed177d3e43");
  const auto memo_directory = restored.find_directory(memo_dir);
  const auto todo_directory = restored.find_directory(todo_dir);
  ASSERT_TRUE(memo_directory.has_value());
  ASSERT_TRUE(todo_directory.has_value());
  EXPECT_EQ(memo_directory->kind, core::DirectoryKind::Memo);
  EXPECT_EQ(memo_directory->name, "研究 / Café");
  EXPECT_EQ(memo_directory->relative_path,
            fs::path{"vault/memo"} / memo_dir.to_string());
  EXPECT_EQ(todo_directory->kind, core::DirectoryKind::Todo);
  EXPECT_EQ(todo_directory->name, "任务");
  const auto memo_children = restored.list_directory(core::DirectoryKind::Memo, memo_dir);
  const auto todo_children = restored.list_directory(core::DirectoryKind::Todo, todo_dir);
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(memo_children));
  ASSERT_TRUE(std::holds_alternative<std::vector<storage::EntryMetadata>>(todo_children));
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(memo_children).size(), 1U);
  EXPECT_EQ(std::get<std::vector<storage::EntryMetadata>>(todo_children).size(), 1U);

  const auto log = restored.find_entry(id("17183823-2574-4bfd-b411-99ed177d3e43"));
  const auto memo = restored.find_entry(id("27183823-2574-4bfd-b411-99ed177d3e43"));
  const auto dated = restored.find_entry(id("37183823-2574-4bfd-b411-99ed177d3e43"));
  const auto timed = restored.find_entry(id("47183823-2574-4bfd-b411-99ed177d3e43"));
  ASSERT_TRUE(log && memo && dated && timed);
  ASSERT_TRUE(std::holds_alternative<storage::LogMetadata>(*log));
  ASSERT_TRUE(std::holds_alternative<storage::MemoMetadata>(*memo));
  ASSERT_TRUE(std::holds_alternative<storage::TodoMetadata>(*dated));
  ASSERT_TRUE(std::holds_alternative<storage::TodoMetadata>(*timed));
  const auto& log_data = std::get<storage::LogMetadata>(*log);
  EXPECT_EQ(log_data.relative_path, fs::path{"vault/log.enc"});
  EXPECT_EQ(log_data.privacy, core::Privacy::Protected);
  EXPECT_EQ(log_data.title, std::string(4096, 'a') + " 旅行");
  EXPECT_EQ(log_data.tags, (std::vector<core::Tag>{{"travel"}, {"旅行"}}));
  EXPECT_EQ(log_data.created.timestamp,
            timestamp_at(std::chrono::seconds{1800000000},
                         std::chrono::nanoseconds{123456000}));
  EXPECT_EQ(log_data.created.timezone, "Asia/Shanghai");
  EXPECT_EQ(log_data.created.location, std::optional<std::string>{"上海"});
  EXPECT_EQ(log_data.modified.timestamp,
            timestamp_at(std::chrono::seconds{1800000090},
                         std::chrono::nanoseconds{987654000}));
  EXPECT_EQ(log_data.modified.timezone, "Europe/Paris");
  EXPECT_EQ(log_data.modified.location, std::nullopt);
  EXPECT_EQ(log_data.log_date.date, std::chrono::year{2027} / 1 / 15);
  EXPECT_EQ(log_data.log_date.timezone, "Asia/Tokyo");
  EXPECT_EQ(log_data.log_date.location, std::optional<std::string>{"東京"});

  const auto& memo_data = std::get<storage::MemoMetadata>(*memo);
  EXPECT_EQ(memo_data.title, "Memo Café");
  EXPECT_EQ(memo_data.tags, (std::vector<core::Tag>{{"notes"}}));
  EXPECT_EQ(memo_data.directory_id, memo_dir);
  EXPECT_EQ(memo_data.privacy, core::Privacy::Normal);
  EXPECT_EQ(memo_data.created.timestamp, log_data.created.timestamp);
  EXPECT_EQ(memo_data.modified.location, std::nullopt);

  const auto& date_data = std::get<storage::TodoMetadata>(*dated);
  EXPECT_EQ(date_data.title, "Todo 日付");
  EXPECT_EQ(date_data.status, core::TodoStatus::Completed);
  EXPECT_EQ(date_data.priority, core::TodoPriority::High);
  EXPECT_EQ(date_data.directory_id, todo_dir);
  EXPECT_EQ(date_data.privacy, core::Privacy::Hidden);
  ASSERT_TRUE(date_data.due_date.has_value());
  ASSERT_TRUE(std::holds_alternative<core::DateContext>(*date_data.due_date));
  const auto& date_due = std::get<core::DateContext>(*date_data.due_date);
  EXPECT_EQ(date_due.date, std::chrono::year{2028} / 2 / 3);
  EXPECT_EQ(date_due.timezone, "Asia/Tokyo");
  EXPECT_EQ(date_due.location, std::optional<std::string>{"東京"});

  const auto& time_data = std::get<storage::TodoMetadata>(*timed);
  EXPECT_EQ(time_data.title, "Todo time");
  EXPECT_EQ(time_data.status, core::TodoStatus::Pending);
  EXPECT_EQ(time_data.priority, core::TodoPriority::Low);
  EXPECT_EQ(time_data.directory_id, std::nullopt);
  ASSERT_TRUE(time_data.due_date.has_value());
  ASSERT_TRUE(std::holds_alternative<core::TimestampContext>(*time_data.due_date));
  const auto& time_due = std::get<core::TimestampContext>(*time_data.due_date);
  EXPECT_EQ(time_due.timestamp,
            timestamp_at(std::chrono::seconds{1900000000},
                         std::chrono::nanoseconds{654321000}));
  EXPECT_EQ(time_due.timezone, "UTC");
  EXPECT_EQ(time_due.location, std::nullopt);

  const auto reencoded = codec::encode_index(restored);
  ASSERT_TRUE(std::holds_alternative<std::string>(reencoded));
  EXPECT_EQ(value_of(reencoded), value_of(encoded));
  auto wire = unpack_wire(value_of(encoded));
  EXPECT_EQ(std::get<std::uint64_t>(field(field(entry(wire), "created"), "nanoseconds").value),
            123456000U);
  EXPECT_FALSE(has_field(field(entry(wire), "modified"), "location"));
  EXPECT_FALSE(has_field(entry(wire, 3), "directory_id"));
}

TEST(IndexCodecTest, RoundTripsEmptyAndEachEntryTypeAlone) {
  storage::MetadataCatalog empty;
  const auto empty_bytes = codec::encode_index(empty);
  ASSERT_TRUE(std::holds_alternative<std::string>(empty_bytes));
  const auto empty_decoded = codec::decode_index(value_of(empty_bytes));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(empty_decoded));
  EXPECT_TRUE(value_of(empty_decoded).query().empty());
  EXPECT_FALSE(value_of(empty_decoded).dirty());

  const auto fixture = make_catalog();
  for (const auto& item : fixture.query()) {
    SCOPED_TRACE(static_cast<int>(storage::entry_type(item)));
    storage::MetadataCatalog one;
    if (const auto* memo = std::get_if<storage::MemoMetadata>(&item)) {
      ASSERT_TRUE(memo->directory_id.has_value());
      ASSERT_TRUE(std::holds_alternative<std::monostate>(one.insert_directory(
          *fixture.find_directory(*memo->directory_id))));
    }
    if (const auto* todo = std::get_if<storage::TodoMetadata>(&item)) {
      if (todo->directory_id) {
        ASSERT_TRUE(std::holds_alternative<std::monostate>(one.insert_directory(
            *fixture.find_directory(*todo->directory_id))));
      }
    }
    ASSERT_TRUE(std::holds_alternative<std::monostate>(one.insert_entry(item)));
    const auto bytes = codec::encode_index(one);
    ASSERT_TRUE(std::holds_alternative<std::string>(bytes));
    const auto decoded = codec::decode_index(value_of(bytes));
    ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
    const auto found = value_of(decoded).find_entry(
        std::visit([](const auto& value) { return value.id; }, item));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(storage::entry_type(*found), storage::entry_type(item));
    EXPECT_EQ(value_of(decoded).query().size(), 1U);
  }
}

TEST(IndexCodecTest, PreservesLongTodoTitleAndNegativeFractionalTimestamp) {
  storage::MetadataCatalog catalog;
  const auto before_epoch = context(
      timestamp_at(std::chrono::seconds{-1},
                   std::chrono::nanoseconds{500000000}),
      "UTC", std::string{});
  const storage::TodoMetadata todo{
      id("58183823-2574-4bfd-b411-99ed177d3e43"), "long-todo.enc",
      core::Privacy::Normal, before_epoch, before_epoch,
      std::string(1u << 20, 'x') + "旅行",
      core::TodoStatus::Pending, core::TodoPriority::None,
      std::nullopt, std::nullopt};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.insert_entry(storage::EntryMetadata{todo})));
  const auto encoded = codec::encode_index(catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  auto wire = unpack_wire(value_of(encoded));
  EXPECT_EQ(std::get<std::int64_t>(
                field(field(entry(wire), "created"), "seconds").value), -1);
  EXPECT_EQ(std::get<std::uint64_t>(
                field(field(entry(wire), "created"), "nanoseconds").value),
            500000000U);
  EXPECT_TRUE(has_field(field(entry(wire), "created"), "location"));
  EXPECT_EQ(std::get<std::string>(
                field(field(entry(wire), "created"), "location").value), "");
  const auto decoded = codec::decode_index(value_of(encoded));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  const auto saved = value_of(decoded).find_entry(todo.id);
  ASSERT_TRUE(saved.has_value());
  const auto& restored = std::get<storage::TodoMetadata>(*saved);
  EXPECT_EQ(restored.title, todo.title);
  EXPECT_EQ(restored.created.timestamp, before_epoch.timestamp);
  EXPECT_EQ(restored.created.location, std::optional<std::string>{""});
}

TEST(IndexCodecTest, EncodingIsIndependentOfCatalogInsertionOrder) {
  const auto first = make_catalog();
  storage::MetadataCatalog reversed;
  const auto todo_dir = first.find_directory(
      id("97183823-2574-4bfd-b411-99ed177d3e43"));
  const auto memo_dir = first.find_directory(
      id("87183823-2574-4bfd-b411-99ed177d3e43"));
  ASSERT_TRUE(todo_dir && memo_dir);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      reversed.insert_directory(*todo_dir)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      reversed.insert_directory(*memo_dir)));
  const auto entries = first.query();
  for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    ASSERT_TRUE(std::holds_alternative<std::monostate>(reversed.insert_entry(*it)));
  const auto forward_bytes = codec::encode_index(first);
  const auto reverse_bytes = codec::encode_index(reversed);
  ASSERT_TRUE(std::holds_alternative<std::string>(forward_bytes));
  ASSERT_TRUE(std::holds_alternative<std::string>(reverse_bytes));
  EXPECT_EQ(value_of(forward_bytes), value_of(reverse_bytes));
  auto wire = unpack_wire(value_of(forward_bytes));
  const auto& packed_entries = std::get<WireValue::Array>(field(wire, "entries").value);
  ASSERT_EQ(packed_entries.size(), 4U);
  EXPECT_EQ(std::get<std::string>(field(entry(wire, 0), "id").value),
            "17183823-2574-4bfd-b411-99ed177d3e43");
  EXPECT_EQ(std::get<std::string>(field(entry(wire, 3), "id").value),
            "47183823-2574-4bfd-b411-99ed177d3e43");
  EXPECT_EQ(std::get<std::string>(field(directory(wire, 0), "id").value),
            "87183823-2574-4bfd-b411-99ed177d3e43");
}

TEST(IndexCodecTest, OmitsAbsentOptionalFieldsAndRejectsExplicitNil) {
  storage::MetadataCatalog catalog;
  const auto created = context(timestamp_at(std::chrono::seconds{1}),
                               "UTC", std::nullopt);
  const storage::TodoMetadata todo{
      id("57183823-2574-4bfd-b411-99ed177d3e43"), "todo.enc",
      core::Privacy::Normal, created, created, "No due",
      core::TodoStatus::Pending, core::TodoPriority::None,
      std::nullopt, std::nullopt};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.insert_entry(storage::EntryMetadata{todo})));
  const auto encoded = codec::encode_index(catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  auto wire = unpack_wire(value_of(encoded));
  EXPECT_FALSE(has_field(entry(wire), "due_date"));
  EXPECT_FALSE(has_field(entry(wire), "directory_id"));
  EXPECT_FALSE(has_field(field(entry(wire), "created"), "location"));
  std::get<WireValue::Map>(entry(wire).value).emplace_back(
      "due_date", WireValue{nullptr});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
}

TEST(IndexCodecTest, PreservesNativeTimestampPrecisionAtMinAndMax) {
  storage::MetadataCatalog catalog;
  const storage::LogMetadata log{
      id("67183823-2574-4bfd-b411-99ed177d3e43"), "extreme.enc",
      core::Privacy::Normal,
      context(core::Timestamp::min(), "UTC", std::nullopt),
      context(core::Timestamp::max(), "UTC", std::nullopt),
      "Extreme", {}, {{std::chrono::year{9999} / 12 / 31}, "UTC", std::nullopt}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      catalog.insert_entry(storage::EntryMetadata{log})));
  const auto encoded = codec::encode_index(catalog);
  ASSERT_TRUE(std::holds_alternative<std::string>(encoded));
  const auto decoded = codec::decode_index(value_of(encoded));
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(decoded));
  const auto restored = value_of(decoded).find_entry(log.id);
  ASSERT_TRUE(restored.has_value());
  const auto& saved = std::get<storage::LogMetadata>(*restored);
  EXPECT_EQ(saved.created.timestamp, core::Timestamp::min());
  EXPECT_EQ(saved.modified.timestamp, core::Timestamp::max());
  EXPECT_EQ(saved.log_date.date, std::chrono::year{9999} / 12 / 31);
}

TEST(IndexCodecTest, RejectsBrokenEnvelopeAndDuplicateKeys) {
  const auto bytes = encoded_catalog();
  expect_error(codec::decode_index(""), codec::IndexCodecErrorCode::Malformed);
  expect_error(codec::decode_index(bytes.substr(0, bytes.size() - 1)),
               codec::IndexCodecErrorCode::Malformed);
  expect_error(codec::decode_index(bytes + std::string{1, '\0'}),
               codec::IndexCodecErrorCode::Malformed);
  expect_error(codec::decode_index(pack_wire(WireValue{std::string{"not a map"}})),
               codec::IndexCodecErrorCode::Malformed);

  auto wire = unpack_wire(bytes);
  erase_field(wire, "schema_version");
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  field(wire, "schema_version") = WireValue{std::uint64_t{2}};
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::UnsupportedVersion);
  wire = unpack_wire(bytes);
  field(wire, "schema_version") = WireValue{std::string{"one"}};
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  field(wire, "entries") = WireValue{std::string{"wrong container"}};
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(wire.value).emplace_back(
      "entries", WireValue{WireValue::Array{}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(entry(wire).value).emplace_back(
      "title", WireValue{std::string{"duplicate"}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(wire.value).emplace_back(
      "future", WireValue{WireValue::Map{{"inner", WireValue{std::uint64_t{1}}}}});
  ASSERT_TRUE(std::holds_alternative<storage::MetadataCatalog>(
      codec::decode_index(pack_wire(wire))));
  std::get<WireValue::Map>(field(wire, "future").value).emplace_back(
      "inner", WireValue{std::uint64_t{2}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
}

TEST(IndexCodecTest, RejectsMissingWrongAndIrrelevantKnownFields) {
  const auto bytes = encoded_catalog();
  for (const auto* key : {"id", "type", "title", "created", "relative_path"}) {
    SCOPED_TRACE(key);
    auto wire = unpack_wire(bytes);
    erase_field(entry(wire), key);
    expect_error(codec::decode_index(pack_wire(wire)),
                 codec::IndexCodecErrorCode::Malformed);
  }
  auto wire = unpack_wire(bytes);
  field(entry(wire), "title") = WireValue{std::uint64_t{42}};
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  field(directory(wire), "kind") = WireValue{std::uint64_t{1}};
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(entry(wire, 2).value).emplace_back(
      "tags", WireValue{WireValue::Array{}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::InvalidMetadata);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(entry(wire, 3).value).emplace_back(
      "directory_id", WireValue{nullptr});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(field(entry(wire), "modified").value).emplace_back(
      "location", WireValue{nullptr});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::Malformed);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(field(entry(wire), "created").value).emplace_back(
      "date", WireValue{std::string{"2027-01-15"}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::InvalidMetadata);
  wire = unpack_wire(bytes);
  std::get<WireValue::Map>(field(entry(wire), "log_date").value).emplace_back(
      "seconds", WireValue{std::int64_t{1}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::InvalidMetadata);
}

TEST(IndexCodecTest, RejectsInvalidMetadataAndRelationshipsWithoutMutatingCaller) {
  auto working = make_catalog();
  working.mark_clean();
  const auto unchanged = codec::encode_index(working);
  ASSERT_TRUE(std::holds_alternative<std::string>(unchanged));
  const auto bytes = value_of(unchanged);
  auto check = [&](WireValue& wire) {
    expect_error(codec::decode_index(pack_wire(wire)),
                 codec::IndexCodecErrorCode::InvalidMetadata);
    EXPECT_FALSE(working.dirty());
    const auto after = codec::encode_index(working);
    ASSERT_TRUE(std::holds_alternative<std::string>(after));
    EXPECT_EQ(value_of(after), bytes);
  };

  auto wire = unpack_wire(bytes);
  field(entry(wire), "privacy") = WireValue{std::string{"Secret"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire), "id") = WireValue{std::string{"not-a-uuid"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire), "relative_path") = WireValue{std::string{"../escape.enc"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire), "relative_path") = WireValue{std::string{"C:/escape.enc"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire), "title") = WireValue{std::string{1, static_cast<char>(0xff)}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 1), "id") = field(entry(wire), "id");
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 1), "relative_path") = field(entry(wire), "relative_path");
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 1), "directory_id") =
      WireValue{std::string{"97183823-2574-4bfd-b411-99ed177d3e43"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 1), "directory_id") =
      WireValue{std::string{"a7183823-2574-4bfd-b411-99ed177d3e43"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 2), "status") = WireValue{std::string{"Doing"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 2), "priority") = WireValue{std::string{"Urgent"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(directory(wire), "kind") = WireValue{std::string{"Archive"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(directory(wire, 1), "id") = field(directory(wire), "id");
  check(wire);
  wire = unpack_wire(bytes);
  field(field(entry(wire), "created"), "nanoseconds") =
      WireValue{std::uint64_t{1000000000}};
  check(wire);
  wire = unpack_wire(bytes);
  field(field(entry(wire), "log_date"), "date") =
      WireValue{std::string{"2027-02-30"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(field(entry(wire, 2), "due_date"), "kind") =
      WireValue{std::string{"TimeOnly"}};
  check(wire);
  wire = unpack_wire(bytes);
  field(entry(wire, 3), "relative_path") =
      WireValue{std::string{"vault\\escape.enc"}};
  check(wire);
}

TEST(IndexCodecTest, EnforcesBinarySizeDepthAndContainerBounds) {
  std::string oversized(64u * 1024u * 1024u + 1, '\0');
  expect_error(codec::decode_index(oversized),
               codec::IndexCodecErrorCode::LimitExceeded);
  const std::string declared_large_array{static_cast<char>(0xdd), '\0', '\1',
                                         static_cast<char>(0x86), static_cast<char>(0xa1)};
  expect_error(codec::decode_index(declared_large_array),
               codec::IndexCodecErrorCode::LimitExceeded);
  const std::string declared_large_map{static_cast<char>(0xde), '\0', 'A'};
  expect_error(codec::decode_index(declared_large_map),
               codec::IndexCodecErrorCode::LimitExceeded);

  auto wire = unpack_wire(encoded_catalog());
  WireValue nested{std::uint64_t{1}};
  for (int depth = 0; depth < 18; ++depth)
    nested = WireValue{WireValue::Array{std::move(nested)}};
  std::get<WireValue::Map>(wire.value).emplace_back("future", std::move(nested));
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::LimitExceeded);

  wire = unpack_wire(encoded_catalog());
  auto& fields = std::get<WireValue::Map>(wire.value);
  for (int i = 0; i < 62; ++i)
    fields.emplace_back("future" + std::to_string(i),
                        WireValue{std::uint64_t{1}});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::LimitExceeded);

  wire = unpack_wire(encoded_catalog());
  auto& entries = std::get<WireValue::Array>(field(wire, "entries").value);
  entries.resize(100001, WireValue{nullptr});
  expect_error(codec::decode_index(pack_wire(wire)),
               codec::IndexCodecErrorCode::LimitExceeded);
}

}  // namespace

