#include "storage/binary_io.hpp"
#include "storage/filesystem.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
namespace fs = std::filesystem;
namespace core = folio::core;
namespace storage = folio::core::storage;

class BinaryIoTest : public testing::Test {
 protected:
  BinaryIoTest() {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      root = fs::temp_directory_path() /
          ("folio-binary-io-test-" + core::Id::generate().to_string());
      if (fs::create_directory(root)) return;
    }
    throw std::runtime_error("Could not reserve test directory");
  }
  ~BinaryIoTest() override {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path root;
};

TEST_F(BinaryIoTest, OpaqueBytesPreserveEmbeddedNulAndExactLimit) {
  const auto file = root / "bytes.bin";
  const std::string bytes{"a\0b\xff", 4};
  const auto created = storage::create_file_atomic(file, bytes);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(created));
  const auto read = storage::read_binary_file(file, bytes.size());
  ASSERT_TRUE(std::holds_alternative<std::string>(read));
  EXPECT_EQ(std::get<std::string>(read), bytes);
  const auto limited = storage::read_binary_file(file, bytes.size() - 1);
  ASSERT_TRUE(std::holds_alternative<storage::BinaryReadError>(limited));
  EXPECT_EQ(std::get<storage::BinaryReadError>(limited).code,
            storage::BinaryReadErrorCode::LimitExceeded);
  const std::string replacement{"\0\0z", 3};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      storage::replace_file_atomic(file, replacement)));
  const auto replaced = storage::read_binary_file(file, replacement.size());
  ASSERT_TRUE(std::holds_alternative<std::string>(replaced));
  EXPECT_EQ(std::get<std::string>(replaced), replacement);
}

TEST_F(BinaryIoTest, MissingAndDirectoryPathsReportIoErrors) {
  const auto missing = storage::read_binary_file(root / "missing.bin", 16);
  ASSERT_TRUE(std::holds_alternative<storage::BinaryReadError>(missing));
  EXPECT_EQ(std::get<storage::BinaryReadError>(missing).code,
            storage::BinaryReadErrorCode::IoError);
  const auto directory = storage::read_binary_file(root, 16);
  ASSERT_TRUE(std::holds_alternative<storage::BinaryReadError>(directory));
  EXPECT_EQ(std::get<storage::BinaryReadError>(directory).code,
            storage::BinaryReadErrorCode::IoError);
}

TEST_F(BinaryIoTest, ZeroLimitAllowsEmptyFileOnly) {
  const auto file = root / "empty.bin";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      storage::create_file_atomic(file, "")));
  const auto empty = storage::read_binary_file(file, 0);
  ASSERT_TRUE(std::holds_alternative<std::string>(empty));
  EXPECT_TRUE(std::get<std::string>(empty).empty());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      storage::replace_file_atomic(file, "x")));
  const auto limited = storage::read_binary_file(file, 0);
  ASSERT_TRUE(std::holds_alternative<storage::BinaryReadError>(limited));
  EXPECT_EQ(std::get<storage::BinaryReadError>(limited).code,
            storage::BinaryReadErrorCode::LimitExceeded);
}

TEST_F(BinaryIoTest, EmbeddedNulPathDoesNotReadTruncatedPrefix) {
  const auto prefix = root / "prefix.bin";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      storage::create_file_atomic(prefix, "sensitive fixture")));
  const fs::path malformed{prefix.string() + std::string{"\0suffix", 7}};
  const auto result = storage::read_binary_file(malformed, 1024);
  ASSERT_TRUE(std::holds_alternative<storage::BinaryReadError>(result));
  EXPECT_EQ(std::get<storage::BinaryReadError>(result).code,
            storage::BinaryReadErrorCode::IoError);
  const auto original = storage::read_binary_file(prefix, 1024);
  ASSERT_TRUE(std::holds_alternative<std::string>(original));
  EXPECT_EQ(std::get<std::string>(original), "sensitive fixture");
}

}  // namespace
