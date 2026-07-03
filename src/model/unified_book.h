#pragma once

#include <optional>

#include "model/order_book.h"

namespace basis::model {

// One event's cross-venue view: the same real-world outcome as priced by
// Kalshi and by Polymarket, each in its own OrderBook. The basis (Kalshi mid
// minus Polymarket mid, in cents) is the divergence the analytics layer
// tracks. Plain data: no callbacks, no threads, no venue JSON.
class UnifiedBook {
 public:
  UnifiedBook() = default;
  // Both venue books draw their level nodes from `mr`.
  explicit UnifiedBook(std::pmr::memory_resource* mr)
      : kalshi_(mr), polymarket_(mr) {}

  void apply(const BookDelta& delta);  // routes on delta.venue

  const OrderBook& book(Venue venue) const;
  std::optional<double> mid(Venue venue) const;

  // Kalshi mid - Polymarket mid, in cents. Empty until both venues have a
  // two-sided book.
  std::optional<double> basis() const;

 private:
  OrderBook kalshi_;
  OrderBook polymarket_;
};

}  // namespace basis::model
