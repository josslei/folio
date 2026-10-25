#include "folio/core/directory.hpp"

#include <utility>

namespace folio::core {

Directory::Directory(std::string name)
    : id_(Id::generate()), name_(std::move(name)) {}

Directory::Directory(const Snapshot& snapshot)
    : id_(snapshot.id), name_(snapshot.name) {}

Directory::Snapshot Directory::snapshot() const { return Snapshot{id_, name_}; }

Directory Directory::restore(const Snapshot& snapshot) {
  return Directory(snapshot);
}

const Id& Directory::id() const noexcept { return id_; }
const std::string& Directory::name() const noexcept { return name_; }

void Directory::set_name(std::string name) { name_ = std::move(name); }

}  // namespace folio::core
