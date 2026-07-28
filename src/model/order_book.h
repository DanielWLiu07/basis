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

  friend std::int64_t crossed_sweep_cents(const OrderBook& rich,
                                          const OrderBook& cheap);
  friend struct SweepFill crossed_sweep_net(const OrderBook& rich,
                                            const OrderBook& cheap,
                                            bool kalshi_is_rich);

 private:
  std::pmr::map<int, std::int64_t, std::greater<int>> bids_;
  std::pmr::map<int, std::int64_t> asks_;
};

// Total capturable edge across the whole crossed depth, in cents: walk
// rich's bids (descending) against cheap's asks (ascending), matching
// contracts while the bid still clears the ask and summing
// (bid - ask) * matched. The touch edge is this sum's first partial term;
// the sweep is what crossing both books to exhaustion was worth. Zero when
// the books do not cross. Sizes never overflow: prices are cents (< 100)
// and venue sizes are far below the int64 range.
std::int64_t crossed_sweep_cents(const OrderBook& rich, const OrderBook& cheap);

// What a fee-aware taker keeps from one crossed sweep, and how many
// contracts were worth taking.
struct SweepFill {
  std::int64_t net_cents = 0;
  std::int64_t contracts = 0;
};

// The optimal fee-aware sweep: walk the crossed depth exactly like
// crossed_sweep_cents, but charge each matched fill the Kalshi taker fee at
// its Kalshi-leg price (the rich bid when Kalshi is rich, the cheap ask when
// it is cheap; the Polymarket leg is fee-free) and take only the fills that
// net strictly positive. Deeper fills clear fewer cents but their fee per
// contract barely moves, so this usually stops well short of the gross
// sweep - the gap between the two is what fees eat. Fees are assessed per
// matched fill; the per-fill ceil never understates what one combined order
// would pay, so the net is conservative. Each fill is kept or dropped
// whole; a partial fill is never split.
SweepFill crossed_sweep_net(const OrderBook& rich, const OrderBook& cheap,
                            bool kalshi_is_rich);

}  // namespace basis::model
