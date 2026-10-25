#include <gtest/gtest.h>

#include <toml++/toml.hpp>

#include <sstream>
#include <string>

TEST(TomlPlusPlusTest, ParsesAccessesAndSerializesBasicValues) {
  const auto document = toml::parse(R"(
title = "A sample memo"
is_private = true
priority = 3
)");

  ASSERT_TRUE(document.contains("title"));
  EXPECT_EQ(document.at("title").value<std::string>(), "A sample memo");
  EXPECT_EQ(document.at("is_private").value<bool>(), true);
  EXPECT_EQ(document.at("priority").value<int>(), 3);

  std::stringstream serialized;
  serialized << document;

  const auto reparsed = toml::parse(serialized.str());
  EXPECT_EQ(reparsed.at("title").value<std::string>(), "A sample memo");
  EXPECT_EQ(reparsed.at("is_private").value<bool>(), true);
  EXPECT_EQ(reparsed.at("priority").value<int>(), 3);
}
