#include "storage/index_store.hpp"

#include <array>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace folio::core::storage {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kMaxIndexBytes = 64u * 1024u * 1024u;  // IndexCodec input limit.
fs::path normal_index_relative_path() { return fs::path{"index"} / "normal.msgpack"; }

IndexStoreError error(IndexStoreErrorCode code, const fs::path& path,
                      std::string message) {
  return {code, path, std::move(message), std::nullopt, std::nullopt};
}
IndexStoreError storage_error(StorageError source) {
  const auto code = source.code == StorageErrorCode::InvalidPath
      ? IndexStoreErrorCode::InvalidPath
      : (source.code == StorageErrorCode::IoError ? IndexStoreErrorCode::IoError
                                                  : IndexStoreErrorCode::StorageFailure);
  IndexStoreError result{code, source.path, source.message, std::move(source), std::nullopt};
  return result;
}
IndexStoreError io_error(const fs::path& path, std::string message,
                         std::error_code ec) {
  return storage_error(StorageError{StorageErrorCode::IoError, path,
                                    std::move(message), ec, false});
}
IndexStoreError codec_error(const fs::path& path, index_codec::IndexCodecError source,
                            bool loading) {
  IndexStoreErrorCode code;
  switch (source.code) {
    case index_codec::IndexCodecErrorCode::UnsupportedVersion:
      code = IndexStoreErrorCode::UnsupportedVersion; break;
    case index_codec::IndexCodecErrorCode::LimitExceeded:
      code = IndexStoreErrorCode::LimitExceeded; break;
    case index_codec::IndexCodecErrorCode::Malformed:
    case index_codec::IndexCodecErrorCode::InvalidMetadata:
      code = loading ? IndexStoreErrorCode::Corrupt : IndexStoreErrorCode::CodecError; break;
  }
  return {code, path, source.message, std::nullopt, std::move(source)};
}

struct Locations {
  fs::path state_root;
  fs::path index_directory;
  fs::path file;
};
IndexStoreResult<Locations> locations(const StoragePaths& paths) {
  auto checked = validate_storage_paths(paths);
  if (auto* problem = std::get_if<StorageError>(&checked)) return storage_error(*problem);
  std::error_code ec;
  auto root = fs::weakly_canonical(paths.state_root, ec);
  if (ec) return io_error(paths.state_root, "Could not resolve state root", ec);
  const auto relative = normal_index_relative_path();
  Locations result{root, root / relative.parent_path(), root / relative};
  return result;
}

IndexStoreResult<fs::file_status> inspect(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory)
    return fs::file_status{fs::file_type::not_found};
  if (ec) return io_error(path, "Could not inspect Index path", ec);
  if (fs::is_symlink(status))
    return error(IndexStoreErrorCode::InvalidPath, path, "Symlink in Index path");
  return status;
}

IndexStoreResult<bool> directory_ready(const fs::path& path) {
  auto result = inspect(path);
  if (auto* problem = std::get_if<IndexStoreError>(&result)) return *problem;
  const auto status = std::get<fs::file_status>(result);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_directory(status))
    return error(IndexStoreErrorCode::InvalidPath, path, "Index parent is not a directory");
  return true;
}
IndexStoreResult<bool> file_ready(const fs::path& path) {
  auto result = inspect(path);
  if (auto* problem = std::get_if<IndexStoreError>(&result)) return *problem;
  const auto status = std::get<fs::file_status>(result);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_regular_file(status))
    return error(IndexStoreErrorCode::InvalidPath, path, "Index target is not a regular file");
  return true;
}
IndexStoreResult<std::string> bounded_read(const fs::path& file) {
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) return io_error(file, "Could not measure Index file", ec);
  if (size > kMaxIndexBytes) {
    auto detail = index_codec::IndexCodecError{
        index_codec::IndexCodecErrorCode::LimitExceeded, "", "Index exceeds 64 MiB"};
    return codec_error(file, std::move(detail), true);
  }
  std::ifstream input(file, std::ios::binary);
  if (!input) return error(IndexStoreErrorCode::IoError, file, "Could not open Index file");
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(size));
  std::array<char, 64u * 1024u> chunk{};
  while (input) {
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = input.gcount();
    if (count > 0) {
      if (static_cast<std::size_t>(count) > kMaxIndexBytes - bytes.size()) {
        auto detail = index_codec::IndexCodecError{
            index_codec::IndexCodecErrorCode::LimitExceeded, "", "Index exceeds 64 MiB"};
        return codec_error(file, std::move(detail), true);
      }
      bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
  }
  if (input.bad() || (input.fail() && !input.eof()))
    return error(IndexStoreErrorCode::IoError, file, "Could not read Index file");
  return bytes;
}
}  // namespace

NormalIndexStore::NormalIndexStore(StoragePaths paths) : paths_(std::move(paths)) {}

IndexStoreResult<MetadataCatalog> NormalIndexStore::load() const {
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<IndexStoreError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.index_directory}) {
    auto ready = directory_ready(directory);
    if (auto* problem = std::get_if<IndexStoreError>(&ready)) return *problem;
    if (!std::get<bool>(ready))
      return error(IndexStoreErrorCode::Missing, path.file, "Normal Index is missing");
  }
  auto ready = file_ready(path.file);
  if (auto* problem = std::get_if<IndexStoreError>(&ready)) return *problem;
  if (!std::get<bool>(ready))
    return error(IndexStoreErrorCode::Missing, path.file, "Normal Index is missing");
  auto bytes = bounded_read(path.file);
  if (auto* problem = std::get_if<IndexStoreError>(&bytes)) return *problem;
  auto decoded = index_codec::decode_index(std::get<std::string>(bytes));
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&decoded))
    return codec_error(path.file, *problem, true);
  return std::get<MetadataCatalog>(std::move(decoded));
}

IndexStoreResult<SaveOutcome> NormalIndexStore::save(const MetadataCatalog& catalog) const {
  if (!catalog.dirty()) return SaveOutcome::SkippedClean;
  auto encoded = index_codec::encode_index(catalog);
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&encoded))
    return codec_error(paths_.state_root / normal_index_relative_path(), *problem, false);
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<IndexStoreError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.index_directory}) {
    auto ready = directory_ready(directory);
    if (auto* problem = std::get_if<IndexStoreError>(&ready)) return *problem;
  }
  // TODO: Directory bootstrap is a separate mutation. A failed file write can
  // leave empty state/index folders; their creation is not separately synced.
  std::error_code ec;
  fs::create_directories(path.index_directory, ec);
  if (ec) return io_error(path.index_directory, "Could not create Index parent", ec);
  for (const auto& directory : {path.state_root, path.index_directory}) {
    auto ready = directory_ready(directory);
    if (auto* problem = std::get_if<IndexStoreError>(&ready)) return *problem;
    if (!std::get<bool>(ready))
      return error(IndexStoreErrorCode::IoError, directory,
                   "Index parent is missing after creation");
  }
  auto ready = file_ready(path.file);
  if (auto* problem = std::get_if<IndexStoreError>(&ready)) return *problem;
  const auto& bytes = std::get<std::string>(encoded);
  auto written = std::get<bool>(ready)
      ? replace_file_atomic(path.file, bytes) : create_file_atomic(path.file, bytes);
  if (auto* problem = std::get_if<StorageError>(&written)) return storage_error(*problem);
  return SaveOutcome::Saved;
}

}  // namespace folio::core::storage
