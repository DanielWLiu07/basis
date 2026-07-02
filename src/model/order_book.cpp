#include "model/order_book.h"

namespace basis::model {

void OrderBook::apply(const BookDelta& delta) {
  auto set_level = [](auto& book, int price, std::int64_t size) {
    if (size <= 0) {
      book.erase(price);
    } else {
      book[price] = size;
    }
  };
  if (delta.side == Side::Bid) {
    set_level(bids_, delta.price_cents, delta.size);
  } else {
    set_level(asks_, delta.price_cents, delta.size);
  }
}

void OrderBook::clear() {
  bids_.clear();
  asks_.clear();
}

std::optional<int> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<int> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::optional<double> OrderBook::mid() const {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) return std::nullopt;
  return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
}

}  // namespace basis::model
