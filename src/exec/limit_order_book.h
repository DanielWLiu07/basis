#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "model/types.h"

namespace basis::exec {

using OrderId = std::uint64_t;

// Prediction-market contracts settle at $0 or $1, so a resting price is an
// integer cent strictly inside that range: 1..99, which is also the implied
// probability. That bound is what shapes this book. A general equity book
// needs a map or tree keyed by an unbounded price; here the whole ladder is
// 99 slots, so price lookup is a direct array index and best-price
// maintenance is a bit scan over two 64-bit words instead of a tree walk.
inline constexpr int kMinPriceCents = 1;
inline constexpr int kMaxPriceCents = 99;

enum class TimeInForce : std::uint8_t {
  Gtc,  // rest whatever does not fill immediately
  Ioc,  // fill what crosses now, cancel the remainder
  Fok,  // fill the whole size immediately or do nothing at all
};

enum class RejectReason : std::uint8_t {
  None,
  PriceOutOfRange,   // outside 1..99
  NonPositiveSize,
  DuplicateOrderId,  // an order with this id is already live
  FokUnfillable,     // Fok could not be filled in full; book untouched
};

// One maker/taker match. Price is the resting (maker) order's price, which
// is the price-time-priority rule: the order that was there first sets the
// terms and the aggressor pays them.
struct Fill {
  OrderId      taker_id    = 0;
  OrderId      maker_id    = 0;
  int          price_cents = 0;
  std::int64_t size        = 0;
};

struct SubmitResult {
  std::int64_t filled  = 0;  // contracts matched immediately
  std::int64_t resting = 0;  // contracts left on the book (Gtc only)
  RejectReason reject  = RejectReason::None;

  bool accepted() const { return reject == RejectReason::None; }
};

// Price-time priority limit order book with matching.
//
// Complexity, all independent of book depth:
//   submit  O(1) per price level touched, O(1) when it rests without crossing
//   cancel  O(1): id -> slab index, then an intrusive unlink
//   best    O(1): a two-word bit scan (std::countl_zero / countr_zero)
//
// Orders live in a slab with a free list, so steady-state add/cancel churn
// allocates nothing: cancelled slots are reused. Each price level is an
// intrusive FIFO of slab indices, which is what makes time priority exact
// and cancellation position-independent.
//
// This is the venue side of the engine. Everything else in basis consumes
// venue data; this produces it, which is what lets the analytics claim
// about executable edge be cross-checked against simulated fills rather
// than trusted.
class LimitOrderBook {
 public:
  LimitOrderBook() = default;

  // Submits an aggressive-or-resting order. Fills are appended to `fills`
  // in match order (never cleared, so a caller can accumulate across
  // submits). A rejected order leaves the book byte-identical.
  SubmitResult submit(OrderId id, model::Side side, int price_cents,
                      std::int64_t size, TimeInForce tif,
                      std::vector<Fill>* fills);

  // Removes a live order. False when the id is unknown (already filled,
  // already cancelled, or never submitted).
  bool cancel(OrderId id);

  // Best resting prices, 0 when that side is empty.
  int best_bid() const;
  int best_ask() const;

  // Aggregate resting size at one price (0 when the level is empty), and
  // across the whole side. These are the aggregated-book view that a venue
  // would publish, so an L2 feed can be checked against this book.
  std::int64_t level_size(model::Side side, int price_cents) const;
  std::int64_t total_size(model::Side side) const;

  std::size_t live_orders() const { return index_of_id_.size(); }
  bool empty() const { return index_of_id_.empty(); }
  void clear();

 private:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  struct Order {
    OrderId       id    = 0;
    std::int64_t  size  = 0;   // remaining, always > 0 while live
    int           price = 0;
    model::Side   side  = model::Side::Bid;
    std::uint32_t next  = kNil;
    std::uint32_t prev  = kNil;
  };

  struct Level {
    std::uint32_t head  = kNil;  // oldest order: next to fill
    std::uint32_t tail  = kNil;  // newest order
    std::int64_t  size  = 0;     // sum of resting sizes at this price
  };

  // One ladder per side, indexed directly by price in cents. Slot 0 and
  // slot 100 stay empty so a price can index without an offset.
  struct Ladder {
    Level        levels[kMaxPriceCents + 1];
    std::uint64_t occupancy[2] = {0, 0};  // bit p set = level p non-empty
    std::int64_t  total = 0;
  };

  Ladder& ladder(model::Side s) {
    return s == model::Side::Bid ? bids_ : asks_;
  }
  const Ladder& ladder(model::Side s) const {
    return s == model::Side::Bid ? bids_ : asks_;
  }

  static void mark(Ladder& l, int price) {
    l.occupancy[price >> 6] |= (1ull << (price & 63));
  }
  static void unmark(Ladder& l, int price) {
    l.occupancy[price >> 6] &= ~(1ull << (price & 63));
  }
  static int highest(const Ladder& l);  // 0 when empty
  static int lowest(const Ladder& l);   // 0 when empty

  std::uint32_t alloc_order();
  void free_order(std::uint32_t idx);
  void link_back(Ladder& l, std::uint32_t idx);
  void unlink(Ladder& l, std::uint32_t idx);

  // Contracts available to an aggressor at `price` or better, capped at
  // `want` so a deep book stops the walk early. Used by Fok's all-or-
  // nothing pre-check, which must not mutate anything.
  std::int64_t crossable_size(model::Side taker_side, int price,
                              std::int64_t want) const;

  Ladder bids_;
  Ladder asks_;
  std::vector<Order>         slab_;
  std::vector<std::uint32_t> free_slots_;
  std::unordered_map<OrderId, std::uint32_t> index_of_id_;
};

}  // namespace basis::exec
