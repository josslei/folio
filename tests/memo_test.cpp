#include "folio/core/memo.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace {

using folio::core::Id;
using folio::core::Memo;
using folio::core::Privacy;
using folio::core::Tag;
using folio::core::Timestamp;
using folio::core::TimestampContext;

TEST(MemoTest, StartsUnfiledWithNormalPrivacyAndCreationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}},
                                 "Asia/Shanghai", std::string("Shanghai")};
  const Memo memo("Idea", "A note", created);
  const Memo another("Idea", "A note", created);

  EXPECT_NE(memo.id(), another.id());
  EXPECT_EQ(memo.title(), "Idea");
  EXPECT_EQ(memo.content(), "A note");
  EXPECT_EQ(memo.created().timestamp, created.timestamp);
  EXPECT_EQ(memo.created().timezone, created.timezone);
  EXPECT_EQ(memo.created().location, created.location);
  EXPECT_EQ(memo.modified().timestamp, created.timestamp);
  EXPECT_EQ(memo.modified().timezone, created.timezone);
  EXPECT_EQ(memo.modified().location, created.location);
  EXPECT_TRUE(memo.tags().empty());
  EXPECT_FALSE(memo.directory_id().has_value());
  EXPECT_EQ(memo.privacy(), Privacy::Normal);
}

TEST(MemoTest, EditsPreserveIdentityAndCreationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  Memo memo("First", "Original", created);
  const Id original_id = memo.id();
  const TimestampContext modified{Timestamp{std::chrono::seconds{200}},
                                  "Europe/Paris", std::string("Paris")};

  memo.edit("Caf\u00e9 idea", "Updated", modified);
  memo.set_tags({Tag{"idea"}, Tag{"work"}}, modified);
  memo.set_privacy(Privacy::Private, modified);

  EXPECT_EQ(memo.id(), original_id);
  EXPECT_EQ(memo.created().timestamp, created.timestamp);
  EXPECT_EQ(memo.created().timezone, created.timezone);
  EXPECT_EQ(memo.created().location, created.location);
  EXPECT_EQ(memo.modified().timestamp, modified.timestamp);
  EXPECT_EQ(memo.modified().timezone, modified.timezone);
  EXPECT_EQ(memo.modified().location, modified.location);
  EXPECT_EQ(memo.title(), "Caf\u00e9 idea");
  EXPECT_EQ(memo.content(), "Updated");
  EXPECT_EQ(memo.tags(), (std::vector<Tag>{Tag{"idea"}, Tag{"work"}}));
  EXPECT_EQ(memo.privacy(), Privacy::Private);
}

TEST(MemoTest, DirectoryCanBeAssignedReplacedAndCleared) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC",
                                 std::nullopt};
  const TimestampContext modified{Timestamp{std::chrono::seconds{200}}, "UTC",
                                  std::nullopt};
  Memo memo("Title", "", created);
  const Id original_id = memo.id();
  const Id first_directory = Id::generate();
  const Id second_directory = Id::generate();

  memo.set_directory_id(first_directory, modified);
  ASSERT_TRUE(memo.directory_id().has_value());
  EXPECT_EQ(*memo.directory_id(), first_directory);

  memo.set_directory_id(second_directory, modified);
  ASSERT_TRUE(memo.directory_id().has_value());
  EXPECT_EQ(*memo.directory_id(), second_directory);

  memo.set_directory_id(std::nullopt, modified);
  EXPECT_FALSE(memo.directory_id().has_value());
  EXPECT_EQ(memo.id(), original_id);
  EXPECT_EQ(memo.created().timestamp, created.timestamp);
  EXPECT_EQ(memo.modified().timestamp, modified.timestamp);
}

}  // namespace
