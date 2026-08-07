#include "exec/limit_order_book.h"

#include <algorithm>
#include <bit>

namespace basis::exec {

namespace {

model::Side opposite(model::Side s) {
  return s == model::Side::Bid ? model::Side::Ask : model::Side::Bid;
}

}  // namespace

int LimitOrderBook::highest(const Ladder& l) {
  // Bit p of word (p >> 6) marks price p, so the highest occupied price is
  // the highest set bit across the two words. Price 0 is never marked, so
  // returning 0 unambiguously means "side empty".
  if (l.occupancy[1] != 0) return 127 - std::countl_zero(l.occupancy[1]);
  if (l.occupancy[0] != 0) return 63 - std::countl_zero(l.occupancy[0]);
  return 0;
}

int LimitOrderBook::lowest(const Ladder& l) {
  if (l.occupancy[0] != 0) return std::countr_zero(l.occupancy[0]);
  if (l.occupancy[1] != 0) return 64 + std::countr_zero(l.occupancy[1]);
  return 0;
}

std::uint32_t LimitOrderBook::alloc_order() {
  if (!free_slots_.empty()) {
    const std::uint32_t idx = free_slots_.back();
    free_slots_.pop_back();
    return idx;
  }
  slab_.emplace_back();
  return static_cast<std::uint32_t>(slab_.size() - 1);
}

void LimitOrderBook::free_order(std::uint32_t idx) {
  slab_[idx] = Order{};
  free_slots_.push_back(idx);
}

void LimitOrderBook::link_back(Ladder& l, std::uint32_t idx) {
  Order& o = slab_[idx];
  Level& lvl = l.levels[o.price];
  o.prev = lvl.tail;
  o.next = kNil;
  if (lvl.tail != kNil) {
    slab_[lvl.tail].next = idx;
  } else {
    lvl.head = idx;      // first order at this price
    mark(l, o.price);
  }
  lvl.tail = idx;
  lvl.size += o.size;
  l.total += o.size;
}

void LimitOrderBook::unlink(Ladder& l, std::uint32_t idx) {
  // Structural only: the caller owns the size accounting, because the fill
  // path decrements as it trades while cancellation removes a whole order.
  Order& o = slab_[idx];
  Level& lvl = l.levels[o.price];
  if (o.prev != kNil) {
    slab_[o.prev].next = o.next;
  } else {
    lvl.head = o.next;
  }
  if (o.next != kNil) {
    slab_[o.next].prev = o.prev;
  } else {
    lvl.tail = o.prev;
  }
  o.next = o.prev = kNil;
  if (lvl.head == kNil) unmark(l, o.price);
}

std::int64_t LimitOrderBook::crossable_size(model::Side taker_side, int price,
                                            std::int64_t want) const {
  const Ladder& opp = ladder(opposite(taker_side));
  std::int64_t have = 0;
  // At most 99 slots, so the straight scan is both cheap and obviously
  // correct; it stops as soon as `want` is covered.
  if (taker_side == model::Side::Bid) {
    for (int p = kMinPriceCents; p <= price && have < want; ++p) {
      have += opp.levels[p].size;
    }
  } else {
    for (int p = kMaxPriceCents; p >= price && have < want; --p) {
      have += opp.levels[p].size;
    }
  }
  return have;
}

SubmitResult LimitOrderBook::submit(OrderId id, model::Side side,
                                    int price_cents, std::int64_t size,
                                    TimeInForce tif,
                                    std::vector<Fill>* fills) {
  SubmitResult result;
  if (price_cents < kMinPriceCents || price_cents > kMaxPriceCents) {
    result.reject = RejectReason::PriceOutOfRange;
    return result;
  }
  if (size <= 0) {
    result.reject = RejectReason::NonPositiveSize;
    return result;
  }
  if (index_of_id_.find(id) != index_of_id_.end()) {
    result.reject = RejectReason::DuplicateOrderId;
    return result;
  }
  // All-or-nothing is decided before anything mutates, so a rejected Fok
  // leaves the book exactly as it was.
  if (tif == TimeInForce::Fok &&
      crossable_size(side, price_cents, size) < size) {
    result.reject = RejectReason::FokUnfillable;
    return result;
  }

  std::int64_t remaining = size;
  Ladder& opp = ladder(opposite(side));
  while (remaining > 0) {
    const int best = (side == model::Side::Bid) ? lowest(opp) : highest(opp);
    if (best == 0) break;  // other side empty
    const bool crosses = (side == model::Side::Bid) ? (best <= price_cents)
                                                    : (best >= price_cents);
    if (!crosses) break;

    Level& lvl = opp.levels[best];
    while (remaining > 0 && lvl.head != kNil) {
      const std::uint32_t maker_idx = lvl.head;
      Order& maker = slab_[maker_idx];
      const std::int64_t traded = std::min(remaining, maker.size);
      if (fills != nullptr) {
        // Price-time priority: the resting order sets the price.
        fills->push_back({id, maker.id, best, traded});
      }
      maker.size -= traded;
      lvl.size   -= traded;
      opp.total  -= traded;
      remaining  -= traded;
      result.filled += traded;
      if (maker.size == 0) {
        const OrderId maker_id = maker.id;
        unlink(opp, maker_idx);
        index_of_id_.erase(maker_id);
        free_order(maker_idx);
      }
    }
  }

  // Ioc and a fully-filled Fok leave nothing behind; Gtc rests the rest.
  if (remaining > 0 && tif == TimeInForce::Gtc) {
    const std::uint32_t idx = alloc_order();
    slab_[idx] = Order{id, remaining, price_cents, side, kNil, kNil};
    link_back(ladder(side), idx);
    index_of_id_.emplace(id, idx);
    result.resting = remaining;
  }
  return result;
}

bool LimitOrderBook::cancel(OrderId id) {
  const auto it = index_of_id_.find(id);
  if (it == index_of_id_.end()) return false;
  const std::uint32_t idx = it->second;
  Ladder& l = ladder(slab_[idx].side);
  const int price = slab_[idx].price;
  const std::int64_t size = slab_[idx].size;
  l.levels[price].size -= size;
  l.total -= size;
  unlink(l, idx);
  index_of_id_.erase(it);
  free_order(idx);
  return true;
}

int LimitOrderBook::best_bid() const { return highest(bids_); }
int LimitOrderBook::best_ask() const { return lowest(asks_); }

std::int64_t LimitOrderBook::level_size(model::Side side,
                                        int price_cents) const {
  if (price_cents < kMinPriceCents || price_cents > kMaxPriceCents) return 0;
  return ladder(side).levels[price_cents].size;
}

std::int64_t LimitOrderBook::total_size(model::Side side) const {
  return ladder(side).total;
}

void LimitOrderBook::reserve(std::size_t orders) {
  slab_.reserve(orders);
  free_slots_.reserve(orders);
  index_of_id_.reserve(orders);
}

void LimitOrderBook::clear() {
  bids_ = Ladder{};
  asks_ = Ladder{};
  slab_.clear();
  free_slots_.clear();
  index_of_id_.clear();
}

}  // namespace basis::exec
