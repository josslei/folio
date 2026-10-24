#include "folio/core/log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using folio::core::DateContext;
using folio::core::Log;
using folio::core::Privacy;
using folio::core::Tag;
using folio::core::Timestamp;
using folio::core::TimestampContext;

TEST(LogTest, StartsWithCreationDateAndPrivatePrivacy) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}},
                                 "Asia/Shanghai", std::string("Shanghai")};
  const DateContext local_date{std::chrono::year{2026} / 10 / 24,
                               "Asia/Shanghai", std::string("Shanghai")};
  const Log log("Journal", "A note", created, local_date);

  EXPECT_TRUE(folio::core::Id::parse(log.id().to_string()).has_value());
  EXPECT_EQ(log.title(), "Journal");
  EXPECT_EQ(log.content(), "A note");
  EXPECT_EQ(log.created().timestamp, created.timestamp);
  EXPECT_EQ(log.modified().timestamp, created.timestamp);
  EXPECT_EQ(log.created().timezone, created.timezone);
  EXPECT_EQ(log.created().location, created.location);
  EXPECT_EQ(log.log_date().date, local_date.date);
  EXPECT_EQ(log.log_date().timezone, local_date.timezone);
  EXPECT_EQ(log.log_date().location, local_date.location);
  EXPECT_TRUE(log.tags().empty());
  EXPECT_EQ(log.privacy(), Privacy::Private);
}

TEST(LogTest, EditsPreserveIdAndCreationContext) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}},
                                 "Asia/Shanghai", std::string("Shanghai")};
  Log log("First", "Original", created,
          {std::chrono::year{2026} / 10 / 24, "Asia/Shanghai", std::string("Shanghai")});
  const auto original_id = log.id();
  const TimestampContext edited{Timestamp{std::chrono::seconds{200}},
                                "Europe/Paris", std::nullopt};

  log.edit("Revised", "Updated", edited);
  log.set_tags({Tag{"idea"}, Tag{"work"}}, edited);
  log.set_privacy(Privacy::Normal, edited);

  EXPECT_EQ(log.id(), original_id);
  EXPECT_EQ(log.created().timestamp, created.timestamp);
  EXPECT_EQ(log.created().timezone, created.timezone);
  EXPECT_EQ(log.created().location, created.location);
  EXPECT_EQ(log.modified().timestamp, edited.timestamp);
  EXPECT_EQ(log.modified().timezone, edited.timezone);
  EXPECT_EQ(log.modified().location, edited.location);
  EXPECT_EQ(log.title(), "Revised");
  EXPECT_EQ(log.content(), "Updated");
  EXPECT_EQ(log.tags(), (std::vector<Tag>{Tag{"idea"}, Tag{"work"}}));
  EXPECT_EQ(log.privacy(), Privacy::Normal);
  EXPECT_EQ(log.log_date().date, std::chrono::year{2026} / 10 / 24);
}

TEST(LogTest, LogDateIsEditableIndependentlyAndCanBeShared) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC", std::nullopt};
  const DateContext creation_date{std::chrono::year{2026} / 10 / 24, "UTC", std::nullopt};
  Log first("First", "", created, creation_date);
  const Log second("Second", "", created, creation_date);
  const auto first_id = first.id();
  const TimestampContext modified{Timestamp{std::chrono::seconds{200}}, "UTC", std::nullopt};

  EXPECT_EQ(first.log_date().date, second.log_date().date);
  first.set_log_date({std::chrono::year{2026} / 10 / 23, "Asia/Shanghai",
                      std::string("Shanghai")}, modified);

  EXPECT_EQ(first.log_date().date, std::chrono::year{2026} / 10 / 23);
  EXPECT_EQ(first.log_date().timezone, "Asia/Shanghai");
  EXPECT_EQ(first.log_date().location, std::optional<std::string>{"Shanghai"});
  EXPECT_EQ(second.log_date().date, creation_date.date);
  EXPECT_EQ(first.id(), first_id);
  EXPECT_EQ(first.created().timestamp, created.timestamp);
  EXPECT_EQ(first.modified().timestamp, modified.timestamp);
}

TEST(LogTest, RejectsInvalidCalendarDatesWithoutChangingLog) {
  const TimestampContext created{Timestamp{std::chrono::seconds{100}}, "UTC", std::nullopt};
  const DateContext invalid_date{std::chrono::year{2026} / 2 / 30, "UTC", std::nullopt};
  EXPECT_THROW(Log("Title", "Content", created, invalid_date), std::invalid_argument);

  Log log("Title", "Content", created,
          {std::chrono::year{2026} / 2 / 28, "UTC", std::nullopt});
  const auto original_id = log.id();
  EXPECT_THROW(log.set_log_date(invalid_date,
                                {Timestamp{std::chrono::seconds{200}}, "UTC", std::nullopt}),
               std::invalid_argument);
  EXPECT_EQ(log.id(), original_id);
  EXPECT_EQ(log.log_date().date, std::chrono::year{2026} / 2 / 28);
  EXPECT_EQ(log.modified().timestamp, created.timestamp);
}

}  // namespace
