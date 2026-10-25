#include "folio/core/memo.hpp"

#include <utility>

namespace folio::core {

Memo::Memo(std::string title, std::string content, TimestampContext created)
    : id_(Id::generate()),
      created_(std::move(created)),
      modified_(created_),
      title_(std::move(title)),
      content_(std::move(content)) {}

const Id& Memo::id() const noexcept { return id_; }
const std::string& Memo::title() const noexcept { return title_; }
const std::string& Memo::content() const noexcept { return content_; }
const TimestampContext& Memo::created() const noexcept { return created_; }
const TimestampContext& Memo::modified() const noexcept { return modified_; }
const std::vector<Tag>& Memo::tags() const noexcept { return tags_; }
const std::optional<Id>& Memo::directory_id() const noexcept { return directory_id_; }
Privacy Memo::privacy() const noexcept { return privacy_; }

void Memo::edit(std::string title, std::string content, TimestampContext modified) {
  title_ = std::move(title);
  content_ = std::move(content);
  modified_ = std::move(modified);
}

void Memo::set_tags(std::vector<Tag> tags, TimestampContext modified) {
  tags_ = std::move(tags);
  modified_ = std::move(modified);
}

void Memo::set_directory_id(std::optional<Id> directory_id,
                            TimestampContext modified) {
  directory_id_ = std::move(directory_id);
  modified_ = std::move(modified);
}

void Memo::set_privacy(Privacy privacy, TimestampContext modified) {
  privacy_ = privacy;
  modified_ = std::move(modified);
}

}  // namespace folio::core
