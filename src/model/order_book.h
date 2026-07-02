#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include "model/book_delta.h"

namespace basis::model {

// One venue's order book for a single market. Bids are kept high-to-low, asks
// low-to-high. A level whose size drops to zero is removed. Prices are cents.
class OrderBook {
 public:
  void apply(const BookDelta& delta);
  void clear();

  std::optional<int> best_bid() const;
  std::optional<int> best_ask() const;
  std::optional<double> mid() const;  // midpoint in cents

  bool empty() const { return bids_.empty() && asks_.empty(); }

 private:
  std::map<int, std::int64_t, std::greater<int>> bids_;
  std::map<int, std::int64_t> asks_;
};

}  // namespace basis::model
