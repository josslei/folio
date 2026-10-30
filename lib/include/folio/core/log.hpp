#pragma once

#include "folio/core/types.hpp"

#include <string>
#include <vector>

namespace folio::core {

class Log {
 public:
  struct Snapshot {
    Id id;
    TimestampContext created;
    TimestampContext modified;
    std::string title;
    std::string content;
    DateContext log_date;
    std::vector<Tag> tags;
    Privacy privacy;
  };

  Log(std::string title, std::string content, TimestampContext created,
      DateContext local_creation_date, Privacy privacy = Privacy::Protected);

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] static Log restore(const Snapshot& snapshot);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] const std::string& title() const noexcept;
  [[nodiscard]] const std::string& content() const noexcept;
  [[nodiscard]] const TimestampContext& created() const noexcept;
  [[nodiscard]] const TimestampContext& modified() const noexcept;
  [[nodiscard]] const DateContext& log_date() const noexcept;
  [[nodiscard]] const std::vector<Tag>& tags() const noexcept;
  [[nodiscard]] Privacy privacy() const noexcept;

  void edit(std::string title, std::string content, TimestampContext modified);
  void set_log_date(DateContext date, TimestampContext modified);
  void set_tags(std::vector<Tag> tags, TimestampContext modified);

 private:
  explicit Log(const Snapshot& snapshot);

  const Id id_;
  const TimestampContext created_;
  TimestampContext modified_;
  std::string title_;
  std::string content_;
  DateContext log_date_;
  std::vector<Tag> tags_;
  const Privacy privacy_;
};

}  // namespace folio::core
