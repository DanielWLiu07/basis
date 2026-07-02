#include "normalize/normalizer.h"

namespace basis::normalize {

bool Normalizer::on_delta(const model::BookDelta& delta) {
  const auto event_id = registry_.event_id(delta.venue, delta.market);
  if (!event_id) {
    ++unmapped_;
    return false;
  }
  auto& book = books_[*event_id];
  book.apply(delta);
  if (observer_) observer_(*event_id, book, delta);
  return true;
}

const model::UnifiedBook* Normalizer::book(const std::string& event_id) const {
  const auto it = books_.find(event_id);
  return it == books_.end() ? nullptr : &it->second;
}

}  // namespace basis::normalize
