#include "model/unified_book.h"

namespace basis::model {

void UnifiedBook::apply(const BookDelta& delta) {
  if (delta.venue == Venue::Kalshi) {
    kalshi_.apply(delta);
  } else {
    polymarket_.apply(delta);
  }
}

const OrderBook& UnifiedBook::book(Venue venue) const {
  return venue == Venue::Kalshi ? kalshi_ : polymarket_;
}

std::optional<double> UnifiedBook::mid(Venue venue) const {
  return book(venue).mid();
}

std::optional<double> UnifiedBook::basis() const {
  const auto k = kalshi_.mid();
  const auto p = polymarket_.mid();
  if (!k || !p) return std::nullopt;
  return *k - *p;
}

}  // namespace basis::model
