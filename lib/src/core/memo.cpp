#include "folio/core/memo.hpp"
#include "internal/validation.hpp"

#include <stdexcept>
#include <utility>

namespace folio::core {

Memo::Memo(std::string title, std::string content, TimestampContext created,
           Privacy privacy)
    : id_(Id::generate()),
      created_(std::move(created)),
      modified_(created_),
      title_(std::move(title)),
      content_(std::move(content)),
      privacy_(privacy) {}

Memo::Memo(const Snapshot& snapshot)
    : id_(snapshot.id),
      created_(snapshot.created),
      modified_(snapshot.modified),
      title_(snapshot.title),
      content_(snapshot.content),
      tags_(snapshot.tags),
      directory_id_(snapshot.directory_id),
      privacy_(snapshot.privacy) {
  if (!detail::is_valid_privacy(privacy_)) {
    throw std::invalid_argument("Memo snapshot privacy is invalid");
  }
}

Memo::Snapshot Memo::snapshot() const {
  return Snapshot{id_, created_, modified_, title_, content_, tags_, directory_id_,
                  privacy_};
}

Memo Memo::restore(const Snapshot& snapshot) { return Memo(snapshot); }

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

}  // namespace folio::core
