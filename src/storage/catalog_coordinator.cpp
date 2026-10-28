#include "storage/catalog_coordinator.hpp"

#include <system_error>
#include <utility>

namespace folio::core::storage {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kMaxIndexBytes = 64u * 1024u * 1024u;  // IndexCodec v1 limit.
fs::path index_relative_path() { return fs::path{"index"} / "normal.msgpack"; }

CoordinatorError error(CoordinatorErrorCode code, const fs::path& path,
                       std::string message) {
  return {code, path, std::move(message), std::nullopt, std::nullopt, std::nullopt};
}
CoordinatorError storage_error(StorageError source) {
  const auto code = source.code == StorageErrorCode::InvalidPath
      ? CoordinatorErrorCode::InvalidPath
      : (source.code == StorageErrorCode::IoError ? CoordinatorErrorCode::IoError
                                                  : CoordinatorErrorCode::StorageFailure);
  return {code, source.path, source.message, std::move(source), std::nullopt, std::nullopt};
}
CoordinatorError io_error(const fs::path& path, std::string message, std::error_code ec) {
  return storage_error(StorageError{StorageErrorCode::IoError, path,
                                    std::move(message), ec, false});
}
CoordinatorError codec_error(const fs::path& path, index_codec::IndexCodecError source,
                             bool loading) {
  CoordinatorErrorCode code;
  switch (source.code) {
    case index_codec::IndexCodecErrorCode::UnsupportedVersion:
      code = CoordinatorErrorCode::UnsupportedVersion; break;
    case index_codec::IndexCodecErrorCode::LimitExceeded:
      code = CoordinatorErrorCode::LimitExceeded; break;
    case index_codec::IndexCodecErrorCode::Malformed:
    case index_codec::IndexCodecErrorCode::InvalidMetadata:
      code = loading ? CoordinatorErrorCode::Corrupt : CoordinatorErrorCode::CodecError; break;
  }
  return {code, path, source.message, std::nullopt, std::move(source), std::nullopt};
}
CoordinatorError binary_error(BinaryReadError source) {
  const auto code = source.code == BinaryReadErrorCode::LimitExceeded
      ? CoordinatorErrorCode::LimitExceeded : CoordinatorErrorCode::IoError;
  return {code, source.path, source.message, std::nullopt, std::nullopt, std::move(source)};
}

struct Locations { fs::path state_root; fs::path parent; fs::path file; };
CoordinatorResult<Locations> locations(const StoragePaths& paths) {
  auto checked = validate_storage_paths(paths);
  if (auto* problem = std::get_if<StorageError>(&checked)) return storage_error(*problem);
  std::error_code ec;
  const auto root = fs::weakly_canonical(paths.state_root, ec);
  if (ec) return io_error(paths.state_root, "Could not resolve state root", ec);
  const auto relative = index_relative_path();
  return Locations{root, root / relative.parent_path(), root / relative};
}
CoordinatorResult<fs::file_status> inspect(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory)
    return fs::file_status{fs::file_type::not_found};
  if (ec) return io_error(path, "Could not inspect Index path", ec);
  if (fs::is_symlink(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Symlink in Index path");
  return status;
}
CoordinatorResult<bool> directory_ready(const fs::path& path) {
  auto inspected = inspect(path);
  if (auto* problem = std::get_if<CoordinatorError>(&inspected)) return *problem;
  const auto status = std::get<fs::file_status>(inspected);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_directory(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Index parent is not a directory");
  return true;
}
CoordinatorResult<bool> file_ready(const fs::path& path) {
  auto inspected = inspect(path);
  if (auto* problem = std::get_if<CoordinatorError>(&inspected)) return *problem;
  const auto status = std::get<fs::file_status>(inspected);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_regular_file(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Index target is not a regular file");
  return true;
}
}  // namespace

CatalogCoordinator::CatalogCoordinator(StoragePaths paths) : paths_(std::move(paths)) {}

CoordinatorResult<std::monostate> CatalogCoordinator::startup_load() {
  if (load_attempted_)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Startup load already attempted");
  load_attempted_ = true;
  status_ = CatalogStatus::Unavailable;
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.parent}) {
    auto ready = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&ready)) return *problem;
    if (!std::get<bool>(ready))
      return error(CoordinatorErrorCode::Missing, path.file, "Normal Index is missing");
  }
  auto ready = file_ready(path.file);
  if (auto* problem = std::get_if<CoordinatorError>(&ready)) return *problem;
  if (!std::get<bool>(ready))
    return error(CoordinatorErrorCode::Missing, path.file, "Normal Index is missing");
  auto bytes = read_binary_file(path.file, kMaxIndexBytes);
  if (auto* problem = std::get_if<BinaryReadError>(&bytes)) return binary_error(*problem);
  auto decoded = index_codec::decode_index(std::get<std::string>(bytes));
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&decoded))
    return codec_error(path.file, *problem, true);
  catalog_ = std::get<MetadataCatalog>(std::move(decoded));
  status_ = CatalogStatus::Ready;
  return std::monostate{};
}

CoordinatorResult<std::vector<ExcludedObject>>
CatalogCoordinator::rebuild() {
  auto paths = validate_storage_paths(paths_);
  if (auto* problem = std::get_if<StorageError>(&paths)) return storage_error(*problem);
  NormalRepository repository(paths_.normal_root);
  auto scanned = repository.scan();
  if (auto* problem = std::get_if<StorageError>(&scanned)) return storage_error(*problem);
  auto inventory = std::get<NormalRepositoryScanResult>(std::move(scanned));
  // Allocate each typed vector once and move records without combining types.
  auto to_vector = []<class T>(std::deque<T>& records) {
    std::vector<T> result;
    result.reserve(records.size());
    for (auto& record : records) result.push_back(std::move(record));
    return result;
  };
  auto adopted = catalog_.rebuild(to_vector(inventory.logs), to_vector(inventory.memos),
                                  to_vector(inventory.todos), to_vector(inventory.directories));
  if (auto* problem = std::get_if<StorageError>(&adopted)) return storage_error(*problem);
  status_ = CatalogStatus::Ready;
  load_attempted_ = true;
  return std::move(inventory.diagnostics);
}

CoordinatorResult<SaveOutcome> CatalogCoordinator::shutdown_save() {
  if (status_ != CatalogStatus::Ready)
    return error(CoordinatorErrorCode::Unavailable, {},
                 "No valid restored catalog is available to save");
  if (!catalog_.unpersisted()) return SaveOutcome::SkippedPersisted;
  auto encoded = index_codec::encode_index(catalog_);
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&encoded))
    return codec_error(paths_.state_root / index_relative_path(), *problem, false);
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.parent}) {
    auto found = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
  }
  // TODO: Parent creation is separate from atomic file publication. A failed
  // write can leave empty folders; their creation is not separately synced.
  std::error_code ec;
  fs::create_directories(path.parent, ec);
  if (ec) return io_error(path.parent, "Could not create Index parent", ec);
  for (const auto& directory : {path.state_root, path.parent}) {
    auto found = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
    if (!std::get<bool>(found))
      return error(CoordinatorErrorCode::IoError, directory,
                   "Index parent is missing after creation");
  }
  auto found = file_ready(path.file);
  if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
  const auto& bytes = std::get<std::string>(encoded);
  auto written = std::get<bool>(found)
      ? replace_file_atomic(path.file, bytes) : create_file_atomic(path.file, bytes);
  if (auto* problem = std::get_if<StorageError>(&written)) return storage_error(*problem);
  catalog_.mark_persisted();
  return SaveOutcome::Saved;
}

const MetadataCatalog& CatalogCoordinator::catalog() const noexcept { return catalog_; }
MetadataCatalog& CatalogCoordinator::catalog() noexcept { return catalog_; }
CatalogStatus CatalogCoordinator::status() const noexcept { return status_; }

}  // namespace folio::core::storage
