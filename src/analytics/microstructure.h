#pragma once

#include <optional>

#include "model/order_book.h"

namespace basis::analytics {

// Queue imbalance at the touch, in [-1, 1]: (bid size - ask size) divided
// by their total. Positive means the bid queue is heavier, which is the
// side more likely to get filled against and the direction price tends to
// drift. nullopt when the book is not two-sided, because a one-sided touch
// has no imbalance to speak of.
inline std::optional<double> queue_imbalance(const model::OrderBook& b) {
  const auto bid_size = b.best_bid_size();
  const auto ask_size = b.best_ask_size();
  if (!bid_size || !ask_size) return std::nullopt;
  const double total = static_cast<double>(*bid_size + *ask_size);
  if (total <= 0.0) return std::nullopt;
  return static_cast<double>(*bid_size - *ask_size) / total;
}

// Microprice in cents: each side's price weighted by the OPPOSITE side's
// size,
//
//   (bid * ask_size + ask * bid_size) / (bid_size + ask_size)
//
// The inversion is the point. A heavy bid queue means buyers are lined up
// and the next trade is more likely to lift the offer, so weight shifts
// toward the ask; the naive mid ignores that and sits in the middle
// regardless. Where the two disagree, the mid is the biased one.
// nullopt when the book is not two-sided.
inline std::optional<double> microprice_cents(const model::OrderBook& b) {
  const auto bid = b.best_bid();
  const auto ask = b.best_ask();
  const auto bid_size = b.best_bid_size();
  const auto ask_size = b.best_ask_size();
  if (!bid || !ask || !bid_size || !ask_size) return std::nullopt;
  const double total = static_cast<double>(*bid_size + *ask_size);
  if (total <= 0.0) return std::nullopt;
  return (static_cast<double>(*bid) * static_cast<double>(*ask_size) +
          static_cast<double>(*ask) * static_cast<double>(*bid_size)) /
         total;
}

}  // namespace basis::analytics
