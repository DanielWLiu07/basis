#include "normalize/normalizer.h"

namespace basis::normalize {

bool Normalizer::on_delta(const model::BookDelta& delta) {
  const auto event_id = registry_.event_id(delta.venue, delta.market);
  if (!event_id) {
    ++unmapped_;
    return false;
  }
  // Heterogeneous find first: in steady state the event exists, so the
  // common case is one hash probe with no key materialization.
  auto it = books_.find(*event_id);
  if (it == books_.end()) {
    it = books_.emplace(std::string(*event_id), model::UnifiedBook(book_mr_))
             .first;
  }
  auto& book = it->second;
  book.apply(delta);
  if (observer_) observer_(it->first, book, delta);
  return true;
}

const model::UnifiedBook* Normalizer::book(std::string_view event_id) const {
  const auto it = books_.find(event_id);
  return it == books_.end() ? nullptr : &it->second;
}

}  // namespace basis::normalize
