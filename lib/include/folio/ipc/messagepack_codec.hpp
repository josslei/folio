#pragma once

#include <folio/ipc/application_protocol.hpp>
#include <folio/ipc/ipc.hpp>
#include <folio/application/mutation.hpp>
#include <msgpack.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

// Extracted from the original read codec. Templates live here so future approved
// payloads can use the same field/type validation, not a parallel serializer.
namespace folio::application::internal::codec {
// Shared positional Application-protocol encoding, independent of persisted schemas.
// Arrays have exact field counts. Optionals/variants are explicitly tagged.
// fields() returns borrowed references for the shared encoder/decoder; their order
// defines the wire layout. Keep the source value alive while traversing the tuple.
// This macro removes repetitive overload boilerplate, not validation or dispatch.
#define FIELDS(Type, ...)                                                                          \
  inline auto fields(const Type& v) {                                                              \
    return std::tie(__VA_ARGS__);                                                                  \
  }
FIELDS(core::LogMetadata, v.id, v.privacy, v.created, v.modified, v.title, v.tags, v.log_date)
FIELDS(core::MemoMetadata, v.id, v.privacy, v.created, v.modified, v.title, v.tags, v.directory_id)
FIELDS(core::TodoMetadata, v.id, v.privacy, v.created, v.modified, v.title, v.status, v.priority,
       v.due_date, v.directory_id)
FIELDS(core::DirectoryMetadata, v.id, v.kind, v.name)
FIELDS(BrowseResult, v.records, v.directories)
FIELDS(core::TimestampContext, v.timestamp, v.timezone, v.location)
FIELDS(core::DateContext, v.date, v.timezone, v.location)
FIELDS(core::Tag, v.value)
FIELDS(core::Subtask, v.content, v.completed)
FIELDS(core::Log::Snapshot, v.id, v.created, v.modified, v.title, v.content, v.log_date, v.tags,
       v.privacy)
FIELDS(core::Memo::Snapshot, v.id, v.created, v.modified, v.title, v.content, v.tags,
       v.directory_id, v.privacy)
FIELDS(core::Todo::Snapshot, v.id, v.created, v.modified, v.title, v.notes, v.status, v.priority,
       v.due_date, v.directory_id, v.privacy, v.subtasks)
FIELDS(RootScope)
FIELDS(AllScope)
FIELDS(DirectoryScope, v.id)
FIELDS(RecordOrder, v.sort, v.descending)
FIELDS(RecordFilters, v.privacy, v.title_contains, v.tag, v.created, v.modified, v.log_date,
       v.todo_status, v.todo_priority, v.has_due_date, v.due_kind, v.due_from, v.due_through)
FIELDS(RecordQuery, v.type, v.scope, v.filters, v.order)
FIELDS(ApplicationError, v.category, v.reason, v.message, v.available_actions)
FIELDS(ApplicationWarning, v.reason, v.message, v.available_actions)
FIELDS(RepositoryScope)
FIELDS(DirectoryScanScope, v.directory, v.traversal)
FIELDS(CheckConsistencyRequest, v.scope)
FIELDS(RefreshRequest, v.scope)
FIELDS(BrowseRequest, v.dir, v.order)
FIELDS(QueryRecordsRequest, v.query)
FIELDS(ListDirectoriesRequest, v.kind)
FIELDS(LoadLogRequest, v.id)
FIELDS(LoadMemoRequest, v.id)
FIELDS(LoadTodoRequest, v.id)
FIELDS(core::Directory::Snapshot, v.id, v.kind, v.name)
FIELDS(std::monostate)
FIELDS(LogEditableFields, v.title, v.content, v.log_date, v.tags)
FIELDS(MemoEditableFields, v.title, v.content, v.tags)
FIELDS(TodoEditableFields, v.title, v.notes, v.status, v.priority, v.due_date, v.subtasks)
FIELDS(CreateLogRequest, v.fields, v.created, v.modified)
FIELDS(CreateMemoRequest, v.fields, v.created, v.modified, v.directory_id)
FIELDS(CreateTodoRequest, v.fields, v.created, v.modified, v.directory_id)
FIELDS(UpdateLogRequest, v.id, v.fields, v.modified)
FIELDS(UpdateMemoRequest, v.id, v.fields, v.modified)
FIELDS(UpdateTodoRequest, v.id, v.fields, v.modified)
FIELDS(MoveRecordRequest, v.id, v.directory_id, v.modified)
FIELDS(MoveMemoRequest, v.id, v.directory_id, v.modified)
FIELDS(MoveTodoRequest, v.id, v.directory_id, v.modified)
FIELDS(CreateDirectoryRequest, v.kind, v.name)
FIELDS(RenameDirectoryRequest, v.id, v.name)
FIELDS(SetTodoStatusRequest, v.id, v.status, v.modified)
FIELDS(MutationRequest, v.mutation_id, v.payload)
FIELDS(AcquireEditRequest, v.id)
FIELDS(ReleaseEditRequest, v.id)
FIELDS(MutationStepEvidence, v.step, v.observation)
FIELDS(MutationEvidence, v.entry_id, v.source, v.destination, v.steps)
FIELDS(MutationFailure, v.error, v.status, v.evidence)
#undef FIELDS

template <class T> auto fields(const InclusiveRange<T>& v) {
  return std::tie(v.from, v.through);
}
template <class T> auto fields(const ApplicationResponse<T>& v) {
  return std::tie(v.value, v.warnings);
}

template <class T> auto fields(const MutationResponse<T>& v) {
  return std::tie(v.mutation_id, v.result);
}

template <class T> struct IsOptional : std::false_type {};

template <class T> struct IsOptional<std::optional<T>> : std::true_type {
  using Value = T;
};
template <class T> struct IsVector : std::false_type {};
template <class T> struct IsVector<std::vector<T>> : std::true_type {
  using Value = T;
};
template <class T> struct IsVariant : std::false_type {};
template <class... T> struct IsVariant<std::variant<T...>> : std::true_type {};

struct Failure : std::exception {
  bool limit;
  explicit Failure(bool resource_limit = false) : limit(resource_limit) {}
};
inline void require(bool condition) {
  if (!condition) throw Failure{};
}

struct Buffer {
  explicit Buffer(bool count_only = false) : count_only(count_only) {}
  std::string bytes;
  std::size_t size = 0;
  bool count_only;
  void write(const char* data, std::size_t count) {
    if (count > max_application_bytes - size) throw Failure{true};
    if (!count_only) bytes.append(data, count);
    size += count;
  }
};
using Packer = msgpack::packer<Buffer>;
using Object = msgpack::object;
constexpr std::size_t max_items = 100000;

// Every enum crossing this private protocol has an explicit upper bound.
constexpr auto last(core::RecordType) {
  return core::RecordType::Todo;
}
constexpr auto last(core::Privacy) {
  return core::Privacy::Hidden;
}
constexpr auto last(core::DirectoryKind) {
  return core::DirectoryKind::Todo;
}
constexpr auto last(core::TodoStatus) {
  return core::TodoStatus::Completed;
}
constexpr auto last(core::TodoPriority) {
  return core::TodoPriority::High;
}
constexpr auto last(DueKind) {
  return DueKind::Datetime;
}
constexpr auto last(MetadataSort) {
  return MetadataSort::Privacy;
}
constexpr auto last(ErrorCategory) {
  return ErrorCategory::ApplicationUnavailable;
}
constexpr auto last(RefreshTraversal) {
  return RefreshTraversal::Recursive;
}
constexpr auto last(ConsistencyCheckResult) {
  return ConsistencyCheckResult::AnomalyDetected;
}
constexpr auto last(ErrorReason) {
  return ErrorReason::FullRefreshRequired;
}
constexpr auto last(ErrorAction) {
  return ErrorAction::Refresh;
}
constexpr auto last(AuthoritativeChangeStatus) {
  return AuthoritativeChangeStatus::Unknown;
}
constexpr auto last(MutationStep) {
  return MutationStep::RemoveSource;
}
constexpr auto last(StepObservation) {
  return StepObservation::Unknown;
}
constexpr auto last(WarningReason) {
  return WarningReason::CatalogInconsistent;
}

inline void array_size(const Object& o, std::size_t size) {
  require(o.type == msgpack::type::ARRAY && o.via.array.size == size);
}

template <class T> void pack(Packer& p, const T& value);
template <class T> T read(const Object& o);

template <class T> void pack(Packer& p, const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    if (value.size() > max_application_bytes) throw Failure{true};
    p.pack_str(static_cast<std::uint32_t>(value.size()));
    p.pack_str_body(value.data(), static_cast<std::uint32_t>(value.size()));
  } else if constexpr (std::is_same_v<T, bool>) {
    p.pack(value);
  } else if constexpr (std::is_enum_v<T>) {
    // Keep malformed caller enums intact so the Backend rejects them as a
    // malformed request; they can never be interpreted as another valid enum.
    p.pack_int64(static_cast<std::int64_t>(value));
  } else if constexpr (std::is_same_v<T, core::Id> || std::is_same_v<T, MutationId>) {
    pack(p, value.to_string());
  } else if constexpr (std::is_same_v<T, core::Timestamp>) {
    p.pack_int64(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
  } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
    p.pack_array(3);
    p.pack_int(int(value.year()));
    p.pack_uint32(unsigned(value.month()));
    p.pack_uint32(unsigned(value.day()));
  } else if constexpr (IsOptional<T>::value) {
    p.pack_array(value ? 1 : 0);
    if (value) pack(p, *value);
  } else if constexpr (IsVector<T>::value) {
    if (value.size() > max_items) throw Failure{true};
    p.pack_array(static_cast<std::uint32_t>(value.size()));
    for (const auto& element : value)
      pack(p, element);
  } else if constexpr (IsVariant<T>::value) {
    p.pack_array(2);
    p.pack_uint64(value.index());
    std::visit([&](const auto& element) { pack(p, element); }, value);
  } else {
    const auto members = fields(value);
    p.pack_array(std::tuple_size_v<decltype(members)>);
    std::apply([&](const auto&... member) { (pack(p, member), ...); }, members);
  }
}

template <class Variant, std::size_t I = 0>
Variant read_variant(std::uint64_t index, const Object& o) {
  if constexpr (I == std::variant_size_v<Variant>) {
    throw Failure{};
  } else {
    if (index == I)
      return Variant{std::in_place_index<I>, read<std::variant_alternative_t<I, Variant>>(o)};
    return read_variant<Variant, I + 1>(index, o);
  }
}

template <class T, std::size_t... I> T read_fields(const Object& o, std::index_sequence<I...>) {
  using Members = decltype(fields(std::declval<const T&>()));
  return T{read<std::remove_cvref_t<std::tuple_element_t<I, Members>>>(o.via.array.ptr[I])...};
}

template <class T> T read(const Object& o) {
  if constexpr (std::is_same_v<T, std::string>) {
    require(o.type == msgpack::type::STR);
    return {o.via.str.ptr, o.via.str.size};
  } else if constexpr (std::is_same_v<T, bool>) {
    require(o.type == msgpack::type::BOOLEAN);
    return o.via.boolean;
  } else if constexpr (std::is_enum_v<T>) {
    require(o.type == msgpack::type::POSITIVE_INTEGER);
    require(o.via.u64 <= static_cast<std::uint64_t>(last(T{})));
    return static_cast<T>(o.via.u64);
  } else if constexpr (std::is_same_v<T, core::Id> || std::is_same_v<T, MutationId>) {
    auto id = T::parse(read<std::string>(o));
    require(id.has_value());
    return *id;
  } else if constexpr (std::is_same_v<T, core::Timestamp>) {
    require(o.type == msgpack::type::NEGATIVE_INTEGER || o.type == msgpack::type::POSITIVE_INTEGER);
    require(o.type != msgpack::type::POSITIVE_INTEGER || o.via.u64 <= std::uint64_t(INT64_MAX));
    const auto ns = o.type == msgpack::type::NEGATIVE_INTEGER
                        ? o.via.i64
                        : static_cast<std::int64_t>(o.via.u64);
    return core::Timestamp{
        std::chrono::duration_cast<core::Timestamp::duration>(std::chrono::nanoseconds{ns})};
  } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
    array_size(o, 3);
    const int year = o.via.array.ptr[0].as<int>();
    const auto month = o.via.array.ptr[1].as<unsigned>();
    const auto day = o.via.array.ptr[2].as<unsigned>();
    require(year >= -32767 && year <= 32767 && month >= 1 && month <= 12 && day >= 1 && day <= 31);
    auto date = std::chrono::year{year} / month / day;
    require(date.ok());
    return date;
  } else if constexpr (IsOptional<T>::value) {
    require(o.type == msgpack::type::ARRAY && o.via.array.size <= 1);
    if (!o.via.array.size) return std::nullopt;
    return read<typename IsOptional<T>::Value>(o.via.array.ptr[0]);
  } else if constexpr (IsVector<T>::value) {
    require(o.type == msgpack::type::ARRAY && o.via.array.size <= max_items);
    T result;
    result.reserve(o.via.array.size);
    for (std::uint32_t i = 0; i < o.via.array.size; ++i)
      result.push_back(read<typename IsVector<T>::Value>(o.via.array.ptr[i]));
    return result;
  } else if constexpr (IsVariant<T>::value) {
    array_size(o, 2);
    require(o.via.array.ptr[0].type == msgpack::type::POSITIVE_INTEGER);
    return read_variant<T>(o.via.array.ptr[0].via.u64, o.via.array.ptr[1]);
  } else {
    constexpr auto size = std::tuple_size_v<decltype(fields(std::declval<const T&>()))>;
    array_size(o, size);
    return read_fields<T>(o, std::make_index_sequence<size>{});
  }
}

// Check the same wire bound without allocating/copying an encoded Record body.
// Edit acquisition uses this before committing a new lock so an oversized
// Snapshot cannot produce a failed response with an undisclosed owned lock.
template <class T> bool fits_message(const T& value) {
  try {
    Buffer buffer(true);
    Packer packer(buffer);
    pack(packer, value);
    return true;
  } catch (...) {
    return false;
  }
}

template <class T> core::Result<std::string, IpcError> encode(const T& value) {
  try {
    Buffer buffer;
    Packer packer(buffer);
    pack(packer, value);
    return std::move(buffer.bytes);
  } catch (const Failure& e) {
    return IpcError{IpcErrorCode::Protocol, e.limit ? "Application message exceeds resource limit"
                                                    : "Invalid Application message"};
  } catch (...) {
    return IpcError{IpcErrorCode::Protocol, "Application message encoding failed"};
  }
}

template <class T> core::Result<T, IpcError> decode(std::string_view bytes) {
  try {
    if (bytes.empty() || bytes.size() > max_application_bytes) throw Failure{};
    std::size_t offset = 0;
    const msgpack::unpack_limit limits{max_items, 0, max_application_bytes, 0, 0, 32};
    auto unpacked = msgpack::unpack(bytes.data(), bytes.size(), offset, nullptr, nullptr, limits);
    require(offset == bytes.size());
    return read<T>(unpacked.get());
  } catch (...) {
    // No parser strings or content bytes cross the public diagnostic boundary.
    return IpcError{IpcErrorCode::Protocol, "Malformed Application message"};
  }
}

} // namespace folio::application::internal::codec
