#include "folio/core/directory.hpp"

#include <stdexcept>
#include <utility>

namespace folio::core {

namespace {
void validate_kind(DirectoryKind kind) {
  if (kind != DirectoryKind::Memo && kind != DirectoryKind::Todo)
    throw std::invalid_argument("Directory kind is invalid");
}
}  // namespace

Directory::Directory(DirectoryKind kind, std::string name)
    : id_(Id::generate()), kind_(kind), name_(std::move(name)) {
  validate_kind(kind_);
}

Directory::Directory(const Snapshot& snapshot)
    : id_(snapshot.id), kind_(snapshot.kind), name_(snapshot.name) {
  validate_kind(kind_);
}

Directory::Snapshot Directory::snapshot() const { return Snapshot{id_, kind_, name_}; }

Directory Directory::restore(const Snapshot& snapshot) {
  return Directory(snapshot);
}

const Id& Directory::id() const noexcept { return id_; }
DirectoryKind Directory::kind() const noexcept { return kind_; }
const std::string& Directory::name() const noexcept { return name_; }

void Directory::set_name(std::string name) { name_ = std::move(name); }

}  // namespace folio::core
