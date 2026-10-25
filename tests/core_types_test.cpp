#include "folio/core/directory.hpp"
#include "folio/core/types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

namespace {

using folio::core::Id;
using folio::core::Directory;

TEST(DirectoryTest, GeneratesDistinctIdsForNewDirectories) {
  const Directory first("Notes");
  const Directory second("Tasks");

  EXPECT_NE(first.id(), second.id());
  EXPECT_TRUE(Id::parse(first.id().to_string()).has_value());
  EXPECT_TRUE(Id::parse(second.id().to_string()).has_value());
}

TEST(DirectoryTest, RenamingPreservesIdentityAndUtf8Name) {
  Directory directory("Notes");
  const Id original_id = directory.id();

  directory.set_name("Caf\u00e9");

  EXPECT_EQ(directory.id(), original_id);
  EXPECT_EQ(directory.name(), "Caf\u00e9");
}

TEST(CoreIdTest, GeneratedIdIsCanonicalUuidV4) {
  const auto id = Id::generate();
  const auto text = id.to_string();

  ASSERT_EQ(text.size(), 36);
  EXPECT_EQ(text[14], '4');
  EXPECT_NE(std::string("89ab").find(text[19]), std::string::npos);
  ASSERT_TRUE(Id::parse(text).has_value());
  EXPECT_EQ(*Id::parse(text), id);
}

TEST(CoreIdTest, ParsesCanonicalUuidV4AndNormalizesCase) {
  const auto id = Id::parse("47183823-2574-4BFD-B411-99ED177D3E43");

  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->to_string(), "47183823-2574-4bfd-b411-99ed177d3e43");
}

TEST(CoreIdTest, RejectsMalformedAndNonV4Identifiers) {
  EXPECT_FALSE(Id::parse("").has_value());
  EXPECT_FALSE(Id::parse("4718382325744bfd-b411-99ed177d3e43").has_value());
  EXPECT_FALSE(Id::parse("47183823-2574-4bfd-b411-99ed177d3e4z").has_value());
  EXPECT_FALSE(Id::parse("47183823-2574-1bfd-b411-99ed177d3e43").has_value());
  EXPECT_FALSE(Id::parse("47183823-2574-4bfd-7411-99ed177d3e43").has_value());
}

TEST(CoreTypesTest, PreservesTimestampAndDateContextAndUtf8Tag) {
  const auto instant = folio::core::Timestamp{std::chrono::seconds{123456789}};
  const folio::core::TimestampContext context{instant, "Asia/Shanghai",
                                               std::string("Shanghai")};
  const folio::core::DateContext date_context{std::chrono::year{2026} / 10 / 24,
                                               "Asia/Shanghai", std::string("Shanghai")};
  const folio::core::Tag tag{std::string("\xE7\xAC\x94\xE8\xAE\xB0")};

  EXPECT_EQ(context.timestamp, instant);
  EXPECT_EQ(context.timezone, "Asia/Shanghai");
  ASSERT_TRUE(context.location.has_value());
  EXPECT_EQ(*context.location, "Shanghai");
  EXPECT_EQ(date_context.date, std::chrono::year{2026} / 10 / 24);
  EXPECT_EQ(date_context.timezone, context.timezone);
  EXPECT_EQ(date_context.location, context.location);
  EXPECT_EQ(tag.value, "\xE7\xAC\x94\xE8\xAE\xB0");
  EXPECT_EQ(tag, folio::core::Tag{tag.value});
  const folio::core::TimestampContext without_location{instant, "UTC", std::nullopt};
  EXPECT_FALSE(without_location.location.has_value());
  EXPECT_NE(folio::core::Privacy::Normal, folio::core::Privacy::Private);
}

}  // namespace
