#include "folio/core/log.hpp"

#include <stdexcept>
#include <utility>

namespace folio::core {

Log::Log(std::string title, std::string content, TimestampContext created,
         DateContext local_creation_date)
    : id_(Id::generate()),
      created_(std::move(created)),
      modified_(created_),
      title_(std::move(title)),
      content_(std::move(content)),
      log_date_(std::move(local_creation_date)) {
  if (!log_date_.date.ok()) {
    throw std::invalid_argument("Log date must be a valid calendar date");
  }
}

const Id& Log::id() const noexcept { return id_; }
const std::string& Log::title() const noexcept { return title_; }
const std::string& Log::content() const noexcept { return content_; }
const TimestampContext& Log::created() const noexcept { return created_; }
const TimestampContext& Log::modified() const noexcept { return modified_; }
const DateContext& Log::log_date() const noexcept { return log_date_; }
const std::vector<Tag>& Log::tags() const noexcept { return tags_; }
Privacy Log::privacy() const noexcept { return privacy_; }

void Log::edit(std::string title, std::string content, TimestampContext modified) {
  title_ = std::move(title);
  content_ = std::move(content);
  modified_ = std::move(modified);
}

void Log::set_log_date(DateContext date, TimestampContext modified) {
  if (!date.date.ok()) {
    throw std::invalid_argument("Log date must be a valid calendar date");
  }
  log_date_ = std::move(date);
  modified_ = std::move(modified);
}

void Log::set_tags(std::vector<Tag> tags, TimestampContext modified) {
  tags_ = std::move(tags);
  modified_ = std::move(modified);
}

void Log::set_privacy(Privacy privacy, TimestampContext modified) {
  privacy_ = privacy;
  modified_ = std::move(modified);
}

}  // namespace folio::core
