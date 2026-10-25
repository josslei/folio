#pragma once

#include "folio/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace folio::core {

class Memo {
 public:
  Memo(std::string title, std::string content, TimestampContext created);

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
  void set_privacy(Privacy privacy, TimestampContext modified);

 private:
  const Id id_;
  const TimestampContext created_;
  TimestampContext modified_;
  std::string title_;
  std::string content_;
  std::vector<Tag> tags_;
  std::optional<Id> directory_id_;
  Privacy privacy_ = Privacy::Normal;
};

}  // namespace folio::core
