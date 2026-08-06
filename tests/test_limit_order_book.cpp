#include <gtest/gtest.h>

#include <vector>

#include "bench/lob_bench.h"
#include "exec/limit_order_book.h"
#include "model/book_delta.h"
#include "model/order_book.h"

using basis::exec::LimitOrderBook;
using basis::exec::Fill;
using basis::exec::RejectReason;
using basis::exec::TimeInForce;
using basis::model::Side;

namespace {

// Convenience: a resting (Gtc) order that is expected to be accepted.
void rest(LimitOrderBook& b, basis::exec::OrderId id, Side side, int price,
          std::int64_t size) {
  std::vector<Fill> fills;
  const auto r = b.submit(id, side, price, size, TimeInForce::Gtc, &fills);
  ASSERT_TRUE(r.accepted());
  ASSERT_TRUE(fills.empty()) << "order " << id << " was expected to rest";
}

}  // namespace

TEST(LimitOrderBook, RestingOrdersSetTheTouchAndDepth) {
  LimitOrderBook book;
  EXPECT_EQ(book.best_bid(), 0);
  EXPECT_EQ(book.best_ask(), 0);
  EXPECT_TRUE(book.empty());

  rest(book, 1, Side::Bid, 45, 100);
  rest(book, 2, Side::Bid, 47, 50);
  rest(book, 3, Side::Ask, 52, 80);

  EXPECT_EQ(book.best_bid(), 47);
  EXPECT_EQ(book.best_ask(), 52);
  EXPECT_EQ(book.level_size(Side::Bid, 45), 100);
  EXPECT_EQ(book.level_size(Side::Bid, 47), 50);
  EXPECT_EQ(book.level_size(Side::Ask, 52), 80);
  EXPECT_EQ(book.total_size(Side::Bid), 150);
  EXPECT_EQ(book.total_size(Side::Ask), 80);
  EXPECT_EQ(book.live_orders(), 3u);
}

TEST(LimitOrderBook, TimePriorityIsFifoWithinAPrice) {
  LimitOrderBook book;
  rest(book, 10, Side::Ask, 50, 30);  // first in
  rest(book, 11, Side::Ask, 50, 30);  // second in
  rest(book, 12, Side::Ask, 50, 30);

  std::vector<Fill> fills;
  const auto r = book.submit(99, Side::Bid, 50, 45, TimeInForce::Gtc, &fills);
  EXPECT_EQ(r.filled, 45);
  EXPECT_EQ(r.resting, 0);
  ASSERT_EQ(fills.size(), 2u);
  // Order 10 arrived first, so it fills first and in full.
  EXPECT_EQ(fills[0].maker_id, 10u);
  EXPECT_EQ(fills[0].size, 30);
  EXPECT_EQ(fills[1].maker_id, 11u);
  EXPECT_EQ(fills[1].size, 15);
  // 11 keeps its queue position with the unfilled remainder; 12 is untouched.
  EXPECT_EQ(book.level_size(Side::Ask, 50), 45);
  EXPECT_EQ(book.live_orders(), 2u);
}

TEST(LimitOrderBook, MakerPriceSetsTheTradePrice) {
  LimitOrderBook book;
  rest(book, 1, Side::Ask, 50, 10);

  std::vector<Fill> fills;
  // Aggressive buy priced at 60 crosses a 50 offer: the resting order was
  // there first, so it sets the terms and the taker gets 10 cents of price
  // improvement rather than paying its own limit.
  const auto r = book.submit(2, Side::Bid, 60, 10, TimeInForce::Gtc, &fills);
  EXPECT_EQ(r.filled, 10);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price_cents, 50);
  EXPECT_EQ(fills[0].taker_id, 2u);
  EXPECT_TRUE(book.empty());
}

TEST(LimitOrderBook, AggressorWalksLevelsThenRestsTheRemainder) {
  LimitOrderBook book;
  rest(book, 1, Side::Ask, 50, 10);
  rest(book, 2, Side::Ask, 51, 10);
  rest(book, 3, Side::Ask, 55, 10);  // above the taker's limit

  std::vector<Fill> fills;
  const auto r = book.submit(9, Side::Bid, 52, 40, TimeInForce::Gtc, &fills);
  EXPECT_EQ(r.filled, 20);   // 50 and 51 only
  EXPECT_EQ(r.resting, 20);  // remainder rests at 52
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].price_cents, 50);  // best price first
  EXPECT_EQ(fills[1].price_cents, 51);
  EXPECT_EQ(book.best_bid(), 52);
  EXPECT_EQ(book.level_size(Side::Bid, 52), 20);
  EXPECT_EQ(book.best_ask(), 55);
}

TEST(LimitOrderBook, CancelUpdatesTheTouchAcrossTheWordBoundary) {
  LimitOrderBook book;
  // Prices 63 and 64 sit in different words of the occupancy bitmap, so
  // this exercises the two-word scan in both directions.
  rest(book, 1, Side::Bid, 63, 10);
  rest(book, 2, Side::Bid, 64, 10);
  rest(book, 3, Side::Ask, 65, 10);
  rest(book, 4, Side::Ask, 70, 10);

  EXPECT_EQ(book.best_bid(), 64);
  EXPECT_TRUE(book.cancel(2));
  EXPECT_EQ(book.best_bid(), 63);   // fell back into the low word
  EXPECT_EQ(book.total_size(Side::Bid), 10);

  EXPECT_EQ(book.best_ask(), 65);
  EXPECT_TRUE(book.cancel(3));
  EXPECT_EQ(book.best_ask(), 70);   // moved up into the high word

  EXPECT_FALSE(book.cancel(2));     // already gone
  EXPECT_FALSE(book.cancel(4242));  // never existed
}

TEST(LimitOrderBook, IocFillsWhatCrossesAndCancelsTheRest) {
  LimitOrderBook book;
  rest(book, 1, Side::Ask, 50, 10);

  std::vector<Fill> fills;
  const auto r = book.submit(2, Side::Bid, 50, 25, TimeInForce::Ioc, &fills);
  EXPECT_EQ(r.filled, 10);
  EXPECT_EQ(r.resting, 0);
  EXPECT_TRUE(book.empty());  // nothing rests, the maker was consumed
}

TEST(LimitOrderBook, FokIsAllOrNothingAndLeavesTheBookUntouchedOnReject) {
  LimitOrderBook book;
  rest(book, 1, Side::Ask, 50, 10);
  rest(book, 2, Side::Ask, 51, 10);

  std::vector<Fill> fills;
  // Wants 25 but only 20 is available at or below 51.
  const auto reject = book.submit(3, Side::Bid, 51, 25, TimeInForce::Fok,
                                  &fills);
  EXPECT_FALSE(reject.accepted());
  EXPECT_EQ(reject.reject, RejectReason::FokUnfillable);
  EXPECT_EQ(reject.filled, 0);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(book.total_size(Side::Ask), 20);  // book untouched
  EXPECT_EQ(book.live_orders(), 2u);

  // Exactly the available size does fill, across both levels.
  const auto ok = book.submit(4, Side::Bid, 51, 20, TimeInForce::Fok, &fills);
  EXPECT_TRUE(ok.accepted());
  EXPECT_EQ(ok.filled, 20);
  EXPECT_EQ(ok.resting, 0);
  EXPECT_EQ(fills.size(), 2u);
  EXPECT_TRUE(book.empty());
}

TEST(LimitOrderBook, RejectsOutOfRangePricesAndBadSizes) {
  LimitOrderBook book;
  std::vector<Fill> fills;
  // Prediction-market contracts settle at 0 or 1, so 0 and 100 are not
  // tradable prices: they are the settlement values themselves.
  EXPECT_EQ(book.submit(1, Side::Bid, 0, 10, TimeInForce::Gtc, &fills).reject,
            RejectReason::PriceOutOfRange);
  EXPECT_EQ(book.submit(2, Side::Bid, 100, 10, TimeInForce::Gtc, &fills).reject,
            RejectReason::PriceOutOfRange);
  EXPECT_EQ(book.submit(3, Side::Bid, 50, 0, TimeInForce::Gtc, &fills).reject,
            RejectReason::NonPositiveSize);
  EXPECT_EQ(book.submit(4, Side::Bid, 50, -5, TimeInForce::Gtc, &fills).reject,
            RejectReason::NonPositiveSize);
  rest(book, 5, Side::Bid, 50, 10);
  EXPECT_EQ(book.submit(5, Side::Bid, 49, 10, TimeInForce::Gtc, &fills).reject,
            RejectReason::DuplicateOrderId);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(book.live_orders(), 1u);
}

TEST(LimitOrderBook, ChurnKeepsAccountingConsistent) {
  // Add and cancel far more orders than the slab starts with, then verify
  // the aggregate view still matches what is live. Slots are recycled, so
  // a stale index or a missed size adjustment shows up here.
  LimitOrderBook book;
  constexpr int kRounds = 500;
  for (int i = 0; i < kRounds; ++i) {
    const int price = 20 + (i % 30);
    rest(book, static_cast<basis::exec::OrderId>(i), Side::Bid, price, 7);
  }
  EXPECT_EQ(book.live_orders(), static_cast<std::size_t>(kRounds));
  EXPECT_EQ(book.total_size(Side::Bid), 7 * kRounds);

  std::int64_t cancelled = 0;
  for (int i = 0; i < kRounds; i += 2) {
    ASSERT_TRUE(book.cancel(static_cast<basis::exec::OrderId>(i)));
    cancelled += 7;
  }
  EXPECT_EQ(book.total_size(Side::Bid), 7 * kRounds - cancelled);

  std::int64_t summed = 0;
  for (int p = basis::exec::kMinPriceCents; p <= basis::exec::kMaxPriceCents;
       ++p) {
    summed += book.level_size(Side::Bid, p);
  }
  EXPECT_EQ(summed, book.total_size(Side::Bid));

  // Sweeping everything with one huge aggressor empties the book exactly.
  std::vector<Fill> fills;
  const auto r = book.submit(999999, Side::Ask, basis::exec::kMinPriceCents,
                             summed, TimeInForce::Ioc, &fills);
  EXPECT_EQ(r.filled, summed);
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.total_size(Side::Bid), 0);
  EXPECT_EQ(book.best_bid(), 0);
}

// The analytics side computes what a sweep is worth by walking two books
// arithmetically (model::crossed_sweep_cents). The matching engine gets
// the same answer by actually executing order flow. Two independent
// implementations of "what does crossing this book pay" must agree, which
// is what turns the reported edge numbers into a checked claim rather than
// a trusted formula.
namespace {

struct Level { int price; std::int64_t size; };

// Runs the arbitrageur's side of the sweep as order flow: rest the cheap
// venue's asks, then hit them with one immediate-or-cancel buy per rich
// bid, best bid first. Returns the realized edge in cents.
std::int64_t simulated_sweep_cents(const std::vector<Level>& rich_bids,
                                   const std::vector<Level>& cheap_asks) {
  LimitOrderBook sim;
  basis::exec::OrderId next_id = 1;
  std::vector<Fill> fills;
  for (const auto& a : cheap_asks) {
    sim.submit(next_id++, Side::Ask, a.price, a.size, TimeInForce::Gtc,
               &fills);
  }
  std::int64_t edge = 0;
  for (const auto& b : rich_bids) {  // callers pass these best-first
    fills.clear();
    sim.submit(next_id++, Side::Bid, b.price, b.size, TimeInForce::Ioc,
               &fills);
    for (const auto& f : fills) {
      // Bought at the resting ask, sold into this bid: the gap is the edge.
      edge += static_cast<std::int64_t>(b.price - f.price_cents) * f.size;
    }
  }
  return edge;
}

basis::model::OrderBook build_book(const std::vector<Level>& levels,
                                   Side side) {
  basis::model::OrderBook book;
  for (const auto& l : levels) {
    basis::model::BookDelta d;
    d.action = basis::model::Action::Set;
    d.side = side;
    d.price_cents = l.price;
    d.size = l.size;
    book.apply(d);
  }
  return book;
}

}  // namespace

TEST(LimitOrderBook, SimulatedSweepMatchesTheAnalyticEdge) {
  // Same fixtures the analytic sweep is pinned against in
  // test_order_book.cpp, so both implementations answer the same cases.
  struct Case {
    const char* name;
    std::vector<Level> rich_bids;   // best first
    std::vector<Level> cheap_asks;  // best first
    std::int64_t want_cents;
  };
  const std::vector<Case> cases = {
      {"single level", {{48, 10}}, {{46, 70}}, 20},
      {"walks past the touch", {{48, 10}, {47, 20}, {45, 500}}, {{46, 70}}, 40},
      {"exhausts an ask level", {{50, 100}}, {{46, 30}, {48, 40}, {51, 999}},
       200},
      {"not crossed", {{45, 100}}, {{46, 70}}, 0},
      {"touching is not crossed", {{45, 100}}, {{45, 30}}, 0},
  };
  for (const auto& c : cases) {
    const auto rich = build_book(c.rich_bids, Side::Bid);
    const auto cheap = build_book(c.cheap_asks, Side::Ask);
    const std::int64_t analytic =
        basis::model::crossed_sweep_cents(rich, cheap);
    const std::int64_t simulated =
        simulated_sweep_cents(c.rich_bids, c.cheap_asks);
    EXPECT_EQ(analytic, c.want_cents) << c.name;
    EXPECT_EQ(simulated, analytic)
        << c.name << ": matching engine and analytic sweep disagree";
  }
}

// Differential test: the production book and the std::map reference in the
// benchmark must produce byte-identical fill streams over a long random
// order flow. This is the property that makes the timing comparison
// meaningful, and it catches matching-semantics drift that the small
// hand-written fixtures above would miss.
TEST(LobBench, LadderAndReferenceBookProduceIdenticalFills) {
  const auto r = basis::bench::run_lob_bench(200'000, 12345);
  EXPECT_TRUE(r.agreed) << "matching engine diverged from the reference book";
  EXPECT_EQ(r.ops, 200'000u);
  EXPECT_GT(r.fills, 0u);          // the flow really does cross
  EXPECT_GT(r.filled_size, 0);
  EXPECT_GT(r.ladder_ns_per_op, 0.0);
  EXPECT_GT(r.map_ns_per_op, 0.0);
}
