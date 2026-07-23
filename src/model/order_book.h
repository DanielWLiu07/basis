#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <optional>

#include "model/book_delta.h"

namespace basis::model {

// One venue's order book for a single market. Bids are kept high-to-low, asks
// low-to-high. A level whose size drops to zero is removed. Prices are cents.
//
// Levels live in node-based maps, so steady-state updates are node churn:
// one allocation per level insert, one free per erase. The constructor takes
// a memory_resource so that churn can come from a pool tuned for fixed-size
// nodes instead of the global heap.
class OrderBook {
 public:
  OrderBook() = default;
  explicit OrderBook(std::pmr::memory_resource* mr) : bids_(mr), asks_(mr) {}

  void apply(const BookDelta& delta);
  void clear();

  std::optional<int> best_bid() const;
  std::optional<int> best_ask() const;
  std::optional<double> mid() const;  // midpoint in cents

  // Size resting at the touch (contracts). nullopt when that side is empty;
  // never zero, because zero-size levels are removed on apply.
  std::optional<std::int64_t> best_bid_size() const;
  std::optional<std::int64_t> best_ask_size() const;

  bool empty() const { return bids_.empty() && asks_.empty(); }

 private:
  std::pmr::map<int, std::int64_t, std::greater<int>> bids_;
  std::pmr::map<int, std::int64_t> asks_;
};

}  // namespace basis::model
