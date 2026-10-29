#include "storage/catalog_coordinator.hpp"
#include "storage/index_codec.hpp"
#include "storage/normal_repository.hpp"

#include <msgpack.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;
namespace codec = folio::core::storage::index_codec;

constexpr std::size_t kIndexLimit = 64u * 1024u * 1024u;

core::Id id(const char* value) {
  const auto parsed = core::Id::parse(value);
  if (!parsed) throw std::runtime_error("Invalid test UUID");
  return *parsed;
}

class IndexResourceLimitsTest : public testing::Test {
 protected:
  IndexResourceLimitsTest() {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = fs::temp_directory_path() /
          ("folio-index-resource-limit-test-" + core::Id::generate().to_string());
      if (fs::create_directory(candidate)) {
        root = fs::canonical(candidate);
        return;
      }
    }
    throw std::runtime_error("Could not reserve resource-limit test directory");
  }
  ~IndexResourceLimitsTest() override {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path root;
  fs::path normal() const { return root / "normal"; }
  fs::path state() const { return root / "state"; }
  fs::path file() const { return root / "cache" / "index" / "normal.msgpack"; }
  storage::StoragePaths paths() const {
    return {normal(), root / "vault", {root / "config", root / "data", state(), root / "cache", std::nullopt}};
  }
  void write_oversized_cache() const {
    ASSERT_TRUE(fs::create_directories(file().parent_path()));
    std::ofstream output(file(), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    const std::string chunk(1024u * 1024u, 'x');
    for (unsigned i = 0; i < 64; ++i) output.write(chunk.data(), chunk.size());
    output.put('Z');
    output.close();
    ASSERT_FALSE(output.fail());
  }
  void expect_cache_unchanged() const {
    ASSERT_EQ(fs::file_size(file()), kIndexLimit + 1);
    std::ifstream input(file(), std::ios::binary);
    ASSERT_TRUE(input.is_open());
    EXPECT_EQ(input.get(), 'x');
    input.seekg(-1, std::ios::end);
    EXPECT_EQ(input.get(), 'Z');
    EXPECT_FALSE(input.bad());
  }
  void expect_oversized_fallback(std::size_t expected_entries) const {
    storage::MetadataCatalog owned_catalog;
    storage::CatalogCoordinator coordinator{paths(), owned_catalog};
    const auto result = coordinator.startup_load();
    ASSERT_TRUE(std::holds_alternative<storage::InitializationOutcome>(result));
    const auto& outcome = std::get<storage::InitializationOutcome>(result);
    ASSERT_TRUE(outcome.index_error.has_value());
    EXPECT_EQ(outcome.index_error->code, storage::CoordinatorErrorCode::LimitExceeded);
    ASSERT_TRUE(outcome.index_error->binary_error.has_value());
    EXPECT_EQ(outcome.index_error->binary_error->code,
              storage::BinaryReadErrorCode::LimitExceeded);
    EXPECT_EQ(outcome.index_error->binary_error->path, file());
    EXPECT_TRUE(outcome.diagnostics.empty());
    EXPECT_EQ(coordinator.status(), storage::CatalogStatus::Ready);
    EXPECT_TRUE(owned_catalog.unpersisted());
    EXPECT_EQ(owned_catalog.query().size(), expected_entries);
    expect_cache_unchanged();
    const auto repeated = coordinator.startup_load();
    ASSERT_TRUE(std::holds_alternative<storage::CoordinatorError>(repeated));
    EXPECT_EQ(std::get<storage::CoordinatorError>(repeated).code,
              storage::CoordinatorErrorCode::Lifecycle);
    expect_cache_unchanged();
  }
};

TEST_F(IndexResourceLimitsTest, DecoderRejectsOversizedInput) {
  std::string bytes(kIndexLimit + 1, '\0');
  const auto decoded = codec::decode_index(bytes);
  ASSERT_TRUE(std::holds_alternative<codec::IndexCodecError>(decoded));
  EXPECT_EQ(std::get<codec::IndexCodecError>(decoded).code,
            codec::IndexCodecErrorCode::LimitExceeded);
}

TEST_F(IndexResourceLimitsTest, DecoderRejects100001MaterializedRecords) {
  msgpack::sbuffer bytes;
  msgpack::packer<msgpack::sbuffer> packer{bytes};
  packer.pack_map(3);
  packer.pack(std::string{"directories"}); packer.pack_array(0);
  packer.pack(std::string{"entries"}); packer.pack_array(100001);
  for (unsigned i = 0; i < 100001; ++i) packer.pack_nil();
  packer.pack(std::string{"schema_version"}); packer.pack_uint64(1);
  const auto decoded = codec::decode_index({bytes.data(), bytes.size()});
  ASSERT_TRUE(std::holds_alternative<codec::IndexCodecError>(decoded));
  EXPECT_EQ(std::get<codec::IndexCodecError>(decoded).code,
            codec::IndexCodecErrorCode::LimitExceeded);
}

TEST_F(IndexResourceLimitsTest, OversizedCacheFallsBackToEmptyNormalStorage) {
  ASSERT_NO_FATAL_FAILURE(write_oversized_cache());
  ASSERT_NO_FATAL_FAILURE(expect_oversized_fallback(0));
  EXPECT_FALSE(fs::exists(normal()));
  EXPECT_FALSE(fs::exists(root / "vault"));
  EXPECT_FALSE(fs::exists(state()));
}

TEST_F(IndexResourceLimitsTest, OversizedCacheFallsBackToNonemptyNormalStorage) {
  storage::NormalRepository repository{normal()};
  const core::TimestampContext created{
      core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", std::nullopt};
  const core::Log::Snapshot log{
      id("a7183823-2574-4bfd-b411-99ed177d3e43"), created, created,
      "Synthetic Log", "body", {{std::chrono::year{2027} / 1 / 15}, "UTC", std::nullopt},
      {{"resource-limit"}}, core::Privacy::Normal};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(repository.create_log(log)));
  ASSERT_NO_FATAL_FAILURE(write_oversized_cache());
  ASSERT_NO_FATAL_FAILURE(expect_oversized_fallback(1));
  EXPECT_FALSE(fs::exists(root / "vault"));
  EXPECT_FALSE(fs::exists(state()));
}

}  // namespace
