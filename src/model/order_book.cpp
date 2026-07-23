#include "model/order_book.h"

#include <limits>

namespace basis::model {

void OrderBook::apply(const BookDelta& delta) {
  if (delta.action == Action::Clear) {
    clear();
    return;
  }
  const auto apply_to = [&delta](auto& book_side) {
    std::int64_t size = delta.size;
    if (delta.action == Action::Add) {
      if (const auto it = book_side.find(delta.price_cents); it != book_side.end()) {
        // Saturate instead of overflowing: a feed corrupt enough to push a
        // level past int64 must not turn into undefined behavior.
        constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
        constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
        const std::int64_t existing = it->second;
        if (size > 0 && existing > kMax - size) {
          size = kMax;
        } else if (size < 0 && existing < kMin - size) {
          size = kMin;  // <= 0, removed below
        } else {
          size += existing;
        }
      }
    }
    if (size <= 0) {
      book_side.erase(delta.price_cents);
    } else {
      book_side[delta.price_cents] = size;
    }
  };
  if (delta.side == Side::Bid) {
    apply_to(bids_);
  } else {
    apply_to(asks_);
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

std::optional<std::int64_t> OrderBook::best_bid_size() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->second;
}

std::optional<std::int64_t> OrderBook::best_ask_size() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->second;
}

std::optional<double> OrderBook::mid() const {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) return std::nullopt;
  return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
}

}  // namespace basis::model
