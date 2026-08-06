#include "bench/lob_bench.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "core/rng.h"
#include "exec/limit_order_book.h"

namespace basis::bench {

namespace {

using exec::LimitOrderBook;
using exec::OrderId;
using exec::TimeInForce;
using model::Side;

// One replayed order-flow operation. Generated once, replayed by both
// implementations, so generation cost is never inside a timed region.
struct Op {
  bool         cancel = false;
  OrderId      id     = 0;
  Side         side   = Side::Bid;
  int          price  = 0;
  std::int64_t size   = 0;
  TimeInForce  tif    = TimeInForce::Gtc;
};

// What a replay produced. Both implementations must agree on all three,
// or the timing comparison is measuring two different workloads.
struct ReplayOutcome {
  std::uint64_t fills = 0;
  std::int64_t  filled_size = 0;
  std::int64_t  notional = 0;  // sum of price * size, in cents
  double        ns = 0.0;
};

// Order flow shaped like a quoting venue: mostly resting quotes near the
// touch and cancels of earlier ones, with a minority of aggressive orders
// that actually cross. The mid random-walks so the working price band
// moves over the run instead of hammering one slot.
std::vector<Op> generate_ops(std::uint64_t n_ops, std::uint32_t seed) {
  std::vector<Op> ops;
  ops.reserve(n_ops);
  std::mt19937 rng(seed);
  std::vector<OrderId> live;
  live.reserve(n_ops / 2);
  OrderId next_id = 1;
  double mid = 50.0;

  for (std::uint64_t i = 0; i < n_ops; ++i) {
    mid += rng::normal(rng, 0.0, 0.35);
    if (mid < 8.0) mid = 8.0;
    if (mid > 92.0) mid = 92.0;
    const int mid_cents = static_cast<int>(mid);
    const double roll = rng::uniform01(rng);
    Op op;
    if (roll < 0.30 && !live.empty()) {
      // Cancel an earlier order. Some of these will miss because the order
      // already filled; both books answer that the same way.
      const auto pick =
          static_cast<std::size_t>(rng::uniform_int(rng, 0, static_cast<std::int64_t>(live.size()) - 1));
      op.cancel = true;
      op.id = live[pick];
      live[pick] = live.back();
      live.pop_back();
    } else if (roll < 0.90) {
      // Passive quote: 1 to 5 cents behind the mid on a random side.
      const bool bid = rng::uniform01(rng) < 0.5;
      const int back = static_cast<int>(rng::uniform_int(rng, 1, 5));
      op.side = bid ? Side::Bid : Side::Ask;
      op.price = bid ? mid_cents - back : mid_cents + back;
      op.size = rng::uniform_int(rng, 1, 200);
      op.tif = TimeInForce::Gtc;
      op.id = next_id++;
      live.push_back(op.id);
    } else {
      // Aggressive: priced through the mid so it crosses whatever is there.
      const bool bid = rng::uniform01(rng) < 0.5;
      const int through = static_cast<int>(rng::uniform_int(rng, 1, 6));
      op.side = bid ? Side::Bid : Side::Ask;
      op.price = bid ? mid_cents + through : mid_cents - through;
      op.size = rng::uniform_int(rng, 1, 400);
      op.tif = TimeInForce::Ioc;
      op.id = next_id++;
    }
    if (op.price < exec::kMinPriceCents) op.price = exec::kMinPriceCents;
    if (op.price > exec::kMaxPriceCents) op.price = exec::kMaxPriceCents;
    ops.push_back(op);
  }
  return ops;
}

// Textbook baseline: a std::map keyed by price with a FIFO list per level,
// which is what this book would be without the bounded-price insight. Same
// semantics as LimitOrderBook, deliberately not the same layout.
class MapBook {
 public:
  struct Resting {
    OrderId      id   = 0;
    std::int64_t size = 0;
  };

  std::int64_t submit(OrderId id, Side side, int price, std::int64_t size,
                      TimeInForce tif, ReplayOutcome* out) {
    if (price < exec::kMinPriceCents || price > exec::kMaxPriceCents) return 0;
    if (size <= 0 || loc_.count(id) != 0) return 0;
    std::int64_t remaining = size;
    if (side == Side::Bid) {
      remaining = match(asks_, remaining, price, true, out);
    } else {
      remaining = match(bids_, remaining, price, false, out);
    }
    if (remaining > 0 && tif == TimeInForce::Gtc) {
      auto& level = (side == Side::Bid) ? bids_[price] : asks_[price];
      level.push_back({id, remaining});
      loc_[id] = {side, price, std::prev(level.end())};
    }
    return remaining;
  }

  bool cancel(OrderId id) {
    const auto it = loc_.find(id);
    if (it == loc_.end()) return false;
    const auto& [side, price, node] = it->second;
    if (side == Side::Bid) {
      auto lit = bids_.find(price);
      lit->second.erase(node);
      if (lit->second.empty()) bids_.erase(lit);
    } else {
      auto lit = asks_.find(price);
      lit->second.erase(node);
      if (lit->second.empty()) asks_.erase(lit);
    }
    loc_.erase(it);
    return true;
  }

 private:
  using Level = std::list<Resting>;
  using AskMap = std::map<int, Level>;
  using BidMap = std::map<int, Level, std::greater<int>>;

  struct Location {
    Side side;
    int price;
    Level::iterator node;
  };

  template <typename BookMap>
  std::int64_t match(BookMap& book, std::int64_t remaining, int limit,
                     bool taker_is_bid, ReplayOutcome* out) {
    while (remaining > 0 && !book.empty()) {
      auto lit = book.begin();  // best price on that side
      const int price = lit->first;
      const bool crosses = taker_is_bid ? (price <= limit) : (price >= limit);
      if (!crosses) break;
      Level& level = lit->second;
      while (remaining > 0 && !level.empty()) {
        Resting& maker = level.front();
        const std::int64_t traded = std::min(remaining, maker.size);
        maker.size -= traded;
        remaining -= traded;
        ++out->fills;
        out->filled_size += traded;
        out->notional += static_cast<std::int64_t>(price) * traded;
        if (maker.size == 0) {
          loc_.erase(maker.id);
          level.pop_front();
        }
      }
      if (level.empty()) book.erase(lit);
    }
    return remaining;
  }

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Location> loc_;
};

ReplayOutcome replay_ladder(const std::vector<Op>& ops) {
  LimitOrderBook book;
  std::vector<exec::Fill> fills;
  fills.reserve(64);
  ReplayOutcome out;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Op& op : ops) {
    if (op.cancel) {
      book.cancel(op.id);
      continue;
    }
    fills.clear();
    book.submit(op.id, op.side, op.price, op.size, op.tif, &fills);
    for (const auto& f : fills) {
      ++out.fills;
      out.filled_size += f.size;
      out.notional += static_cast<std::int64_t>(f.price_cents) * f.size;
    }
  }
  out.ns = static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0).count());
  return out;
}

ReplayOutcome replay_map(const std::vector<Op>& ops) {
  MapBook book;
  ReplayOutcome out;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Op& op : ops) {
    if (op.cancel) {
      book.cancel(op.id);
      continue;
    }
    book.submit(op.id, op.side, op.price, op.size, op.tif, &out);
  }
  out.ns = static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0).count());
  return out;
}

}  // namespace

LobBenchResult run_lob_bench(std::uint64_t n_ops, std::uint32_t seed) {
  const auto ops = generate_ops(n_ops, seed);
  // Run each once to warm the allocator and caches, then measure.
  (void)replay_ladder(ops);
  (void)replay_map(ops);
  const ReplayOutcome ladder = replay_ladder(ops);
  const ReplayOutcome map = replay_map(ops);

  LobBenchResult r;
  r.ops = ops.size();
  r.fills = ladder.fills;
  r.filled_size = ladder.filled_size;
  r.agreed = ladder.fills == map.fills &&
             ladder.filled_size == map.filled_size &&
             ladder.notional == map.notional;
  const double n = static_cast<double>(ops.size());
  r.ladder_ns_per_op = ladder.ns / n;
  r.map_ns_per_op = map.ns / n;
  r.speedup = ladder.ns > 0.0 ? map.ns / ladder.ns : 0.0;
  return r;
}

}  // namespace basis::bench
