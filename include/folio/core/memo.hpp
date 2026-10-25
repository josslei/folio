#pragma once

#include "folio/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace folio::core {

class Memo {
 public:
  struct Snapshot {
    Id id;
    TimestampContext created;
    TimestampContext modified;
    std::string title;
    std::string content;
    std::vector<Tag> tags;
    std::optional<Id> directory_id;
    Privacy privacy;
  };

  Memo(std::string title, std::string content, TimestampContext created,
       Privacy privacy = Privacy::Normal);

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] static Memo restore(const Snapshot& snapshot);

  [[nodiscard]] const Id& id() const noexcept;
  [[nodiscard]] const std::string& title() const noexcept;
  [[nodiscard]] const std::string& content() const noexcept;
  [[nodiscard]] const TimestampContext& created() const noexcept;
  [[nodiscard]] const TimestampContext& modified() const noexcept;
  [[nodiscard]] const std::vector<Tag>& tags() const noexcept;
  [[nodiscard]] const std::optional<Id>& directory_id() const noexcept;
  [[nodiscard]] Privacy privacy() const noexcept;

  void edit(std::string title, std::string content, TimestampContext modified);
  void set_tags(std::vector<Tag> tags, TimestampContext modified);
  void set_directory_id(std::optional<Id> directory_id, TimestampContext modified);

 private:
  explicit Memo(const Snapshot& snapshot);

  const Id id_;
  const TimestampContext created_;
  TimestampContext modified_;
  std::string title_;
  std::string content_;
  std::vector<Tag> tags_;
  std::optional<Id> directory_id_;
  const Privacy privacy_;
};

}  // namespace folio::core
