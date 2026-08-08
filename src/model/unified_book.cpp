#include "model/unified_book.h"

namespace basis::model {

void UnifiedBook::apply(const BookDelta& delta) {
  books_[index_of(delta.venue)].apply(delta);
}

const OrderBook& UnifiedBook::book(Venue venue) const {
  return books_[index_of(venue)];
}

std::optional<double> UnifiedBook::mid(Venue venue) const {
  return book(venue).mid();
}

std::optional<double> UnifiedBook::basis() const {
  const auto k = books_[index_of(Venue::Kalshi)].mid();
  const auto p = books_[index_of(Venue::Polymarket)].mid();
  if (!k || !p) return std::nullopt;
  return *k - *p;
}

}  // namespace basis::model
