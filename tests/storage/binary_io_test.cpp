#include "storage/common.hpp"
#include <folio/io/filesystem.hpp>
#include <array>

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
      root =
          fs::temp_directory_path() / ("folio-binary-io-test-" + core::Id::generate().to_string());
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
  const auto created = folio::io::create_file_atomic(file, bytes);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(created));
  const auto read = folio::io::read_file(file, bytes.size());
  ASSERT_TRUE(std::holds_alternative<std::string>(read));
  EXPECT_EQ(std::get<std::string>(read), bytes);
  const auto limited = folio::io::read_file(file, bytes.size() - 1);
  ASSERT_TRUE(std::holds_alternative<folio::io::ReadError>(limited));
  EXPECT_EQ(std::get<folio::io::ReadError>(limited).code, folio::io::ReadErrorCode::LimitExceeded);
  const std::string replacement{"\0\0z", 3};
  ASSERT_TRUE(
      std::holds_alternative<std::monostate>(folio::io::replace_file_atomic(file, replacement)));
  const auto replaced = folio::io::read_file(file, replacement.size());
  ASSERT_TRUE(std::holds_alternative<std::string>(replaced));
  EXPECT_EQ(std::get<std::string>(replaced), replacement);
}

TEST_F(BinaryIoTest, MissingAndDirectoryPathsReportIoErrors) {
  const auto missing = folio::io::read_file(root / "missing.bin", 16);
  ASSERT_TRUE(std::holds_alternative<folio::io::ReadError>(missing));
  EXPECT_EQ(std::get<folio::io::ReadError>(missing).code, folio::io::ReadErrorCode::IoError);
  const auto directory = folio::io::read_file(root, 16);
  ASSERT_TRUE(std::holds_alternative<folio::io::ReadError>(directory));
  EXPECT_EQ(std::get<folio::io::ReadError>(directory).code, folio::io::ReadErrorCode::IoError);
}

TEST_F(BinaryIoTest, ZeroLimitAllowsEmptyFileOnly) {
  const auto file = root / "empty.bin";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::create_file_atomic(file, "")));
  const auto empty = folio::io::read_file(file, 0);
  ASSERT_TRUE(std::holds_alternative<std::string>(empty));
  EXPECT_TRUE(std::get<std::string>(empty).empty());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(folio::io::replace_file_atomic(file, "x")));
  const auto limited = folio::io::read_file(file, 0);
  ASSERT_TRUE(std::holds_alternative<folio::io::ReadError>(limited));
  EXPECT_EQ(std::get<folio::io::ReadError>(limited).code, folio::io::ReadErrorCode::LimitExceeded);
}

TEST_F(BinaryIoTest, EmbeddedNulPathDoesNotReadTruncatedPrefix) {
  const auto prefix = root / "prefix.bin";
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      folio::io::create_file_atomic(prefix, "sensitive fixture")));
  const fs::path malformed{prefix.string() + std::string{"\0suffix", 7}};
  const auto result = folio::io::read_file(malformed, 1024);
  ASSERT_TRUE(std::holds_alternative<folio::io::ReadError>(result));
  EXPECT_EQ(std::get<folio::io::ReadError>(result).code, folio::io::ReadErrorCode::IoError);
  const auto original = folio::io::read_file(prefix, 1024);
  ASSERT_TRUE(std::holds_alternative<std::string>(original));
  EXPECT_EQ(std::get<std::string>(original), "sensitive fixture");
}

TEST_F(BinaryIoTest, ByteSpanWritesAndStringViewsShareAtomicPreconditions) {
  namespace io = folio::io;
  const auto file = root / "span.bin";
  const std::array bytes{std::byte{0}, std::byte{0xff}, std::byte{0x41}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      io::create_file_atomic(file, std::span<const std::byte>{bytes})));
  auto result = io::read_file(file, bytes.size());
  ASSERT_TRUE(std::holds_alternative<std::string>(result));
  EXPECT_EQ(std::get<std::string>(result), (std::string{"\0\xff"
                                                        "A",
                                                        3}));
  auto duplicate = io::create_file_atomic(file, "must not overwrite");
  ASSERT_TRUE(std::holds_alternative<io::FileError>(duplicate));
  EXPECT_EQ(std::get<io::FileError>(duplicate).system_error,
            std::make_error_code(std::errc::file_exists));
  EXPECT_FALSE(std::get<io::FileError>(duplicate).replacement_completed);
  auto missing = io::replace_file_atomic(root / "missing", std::span<const std::byte>{bytes});
  ASSERT_TRUE(std::holds_alternative<io::FileError>(missing));
  EXPECT_EQ(std::get<io::FileError>(missing).system_error,
            std::make_error_code(std::errc::no_such_file_or_directory));
  EXPECT_FALSE(std::get<io::FileError>(missing).replacement_completed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      io::replace_file_atomic(file, std::string_view{"text\0bytes", 10})));
  auto replaced = io::read_file(file, 10);
  ASSERT_TRUE(std::holds_alternative<std::string>(replaced));
  EXPECT_EQ(std::get<std::string>(replaced), (std::string{"text\0bytes", 10}));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      io::replace_file_atomic(file, std::span<const std::byte>{})));
  auto empty = io::read_file(file, 0);
  ASSERT_TRUE(std::holds_alternative<std::string>(empty));
  EXPECT_TRUE(std::get<std::string>(empty).empty());
}

} // namespace
