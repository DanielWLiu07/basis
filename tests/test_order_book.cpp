#include <gtest/gtest.h>

#include <limits>

#include "model/order_book.h"

using basis::model::Action;
using basis::model::BookDelta;
using basis::model::OrderBook;
using basis::model::Side;
using basis::model::Venue;

namespace {

BookDelta delta(Side side, int price, std::int64_t size,
                Action action = Action::Set) {
  return BookDelta{.venue = Venue::Kalshi,
                   .market = "TEST",
                   .action = action,
                   .side = side,
                   .price_cents = price,
                   .size = size};
}

}  // namespace

TEST(OrderBook, EmptyHasNoMid) {
  OrderBook b;
  EXPECT_TRUE(b.empty());
  EXPECT_FALSE(b.mid().has_value());
}

TEST(OrderBook, MidIsMeanOfBestBidAsk) {
  OrderBook b;
  b.apply(delta(Side::Bid, 47, 1200));
  b.apply(delta(Side::Ask, 49, 800));
  ASSERT_TRUE(b.mid().has_value());
  EXPECT_DOUBLE_EQ(*b.mid(), 48.0);
  EXPECT_EQ(*b.best_bid(), 47);
  EXPECT_EQ(*b.best_ask(), 49);
}

TEST(OrderBook, BestBidTracksHighestBestAskTracksLowest) {
  OrderBook b;
  b.apply(delta(Side::Bid, 40, 100));
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Bid, 42, 100));
  b.apply(delta(Side::Ask, 55, 100));
  b.apply(delta(Side::Ask, 51, 100));
  EXPECT_EQ(*b.best_bid(), 45);
  EXPECT_EQ(*b.best_ask(), 51);
}

TEST(OrderBook, TouchSizesFollowTheBestLevels) {
  OrderBook b;
  EXPECT_FALSE(b.best_bid_size().has_value());
  EXPECT_FALSE(b.best_ask_size().has_value());
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Bid, 47, 25));
  b.apply(delta(Side::Ask, 51, 70));
  EXPECT_EQ(*b.best_bid_size(), 25);   // the 47 level, not the deeper 45
  EXPECT_EQ(*b.best_ask_size(), 70);
  b.apply(delta(Side::Bid, 47, 0));    // pull the top: size follows to 45
  EXPECT_EQ(*b.best_bid_size(), 100);
}

TEST(OrderBook, ZeroSizeRemovesLevel) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Bid, 47, 100));
  EXPECT_EQ(*b.best_bid(), 47);
  b.apply(delta(Side::Bid, 47, 0));  // pull the top level
  EXPECT_EQ(*b.best_bid(), 45);
}

TEST(OrderBook, AddAccumulatesOnExistingLevel) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Bid, 45, 50, Action::Add));
  b.apply(delta(Side::Bid, 45, -30, Action::Add));
  // 100 + 50 - 30 = 120 resting; the level is still the best bid.
  EXPECT_EQ(*b.best_bid(), 45);
  b.apply(delta(Side::Bid, 45, -120, Action::Add));
  EXPECT_FALSE(b.best_bid().has_value());
}

TEST(OrderBook, AddOnMissingLevelCreatesIt) {
  OrderBook b;
  b.apply(delta(Side::Ask, 52, 75, Action::Add));
  EXPECT_EQ(*b.best_ask(), 52);
}

TEST(OrderBook, AddBelowZeroRemovesLevel) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 10));
  b.apply(delta(Side::Bid, 45, -25, Action::Add));  // over-remove: clamp to gone
  EXPECT_TRUE(b.empty());
}

TEST(OrderBook, AddSaturatesInsteadOfOverflowing) {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  OrderBook b;
  b.apply(delta(Side::Bid, 45, kMax));
  b.apply(delta(Side::Bid, 45, kMax, Action::Add));  // would overflow: clamp
  EXPECT_EQ(*b.best_bid(), 45);
  b.apply(delta(Side::Bid, 45, -kMax, Action::Add));
  b.apply(delta(Side::Bid, 45, -kMax, Action::Add));  // saturates low, removes
  EXPECT_TRUE(b.empty());
}

TEST(OrderBook, ClearActionEmptiesBook) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Ask, 55, 100));
  b.apply(delta(Side::Bid, 0, 0, Action::Clear));
  EXPECT_TRUE(b.empty());
}

TEST(OrderBook, ClearEmptiesBook) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Ask, 55, 100));
  b.clear();
  EXPECT_TRUE(b.empty());
}

TEST(CrossedSweep, ZeroWhenNotCrossedOrEmpty) {
  OrderBook rich, cheap;
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 0);
  rich.apply(delta(Side::Bid, 45, 100));
  cheap.apply(delta(Side::Ask, 46, 70));
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 0);  // 45 bid < 46 ask
  cheap.apply(delta(Side::Ask, 45, 30));
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 0);  // touching is not crossed
}

TEST(CrossedSweep, SingleLevelEqualsTouchEdge) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 48, 10));
  cheap.apply(delta(Side::Ask, 46, 70));
  // One level each: the sweep is exactly depth * min(sizes) = 2c * 10.
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 20);
}

TEST(CrossedSweep, WalksPastTheTouchAndStopsAtTheUncross) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 48, 10));
  rich.apply(delta(Side::Bid, 47, 20));
  rich.apply(delta(Side::Bid, 45, 500));  // below the ask: never matched
  cheap.apply(delta(Side::Ask, 46, 70));
  // 48x10 fills against 46 (2c * 10), then 47x20 against the same level's
  // remainder (1c * 20); the 45 bid no longer clears 46 and the walk stops.
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 20 + 20);
}

TEST(CrossedSweep, ExhaustsAnAskLevelAndContinuesDeeper) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 50, 100));
  cheap.apply(delta(Side::Ask, 46, 30));
  cheap.apply(delta(Side::Ask, 48, 40));
  cheap.apply(delta(Side::Ask, 51, 999));  // above the bid: never matched
  // One big bid sweeps two ask levels: 4c * 30 + 2c * 40, then 51 uncrosses.
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 120 + 80);
}

TEST(CrossedSweepNet, TakesNothingWhenNotCrossed) {
  OrderBook rich, cheap;
  auto fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(fill.net_cents, 0);
  EXPECT_EQ(fill.contracts, 0);
  rich.apply(delta(Side::Bid, 45, 100));
  cheap.apply(delta(Side::Ask, 45, 30));  // touching is not crossed
  fill = crossed_sweep_net(rich, cheap, false);
  EXPECT_EQ(fill.net_cents, 0);
  EXPECT_EQ(fill.contracts, 0);
}

TEST(CrossedSweepNet, KeepsAFillThatClearsItsFee) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 48, 10));
  cheap.apply(delta(Side::Ask, 46, 70));
  // Gross 2c * 10 = 20c; Kalshi leg at the rich bid pays fee(10, 48) = 18c.
  const auto fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(fill.net_cents, 2);
  EXPECT_EQ(fill.contracts, 10);
}

TEST(CrossedSweepNet, DeclinesAFillTheFeeEats) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 47, 25));
  cheap.apply(delta(Side::Ask, 46, 25));
  // Gross 1c * 25 = 25c but fee(25, 47) = 44c: a taker declines to trade.
  auto fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(fill.net_cents, 0);
  EXPECT_EQ(fill.contracts, 0);
  // Breaking exactly even is also declining: gross 2c, fee(1, 50) = 2c.
  rich.clear();
  cheap.clear();
  rich.apply(delta(Side::Bid, 50, 1));
  cheap.apply(delta(Side::Ask, 48, 1));
  fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(fill.net_cents, 0);
  EXPECT_EQ(fill.contracts, 0);
}

TEST(CrossedSweepNet, StopsShortOfTheGrossSweep) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 48, 10));
  rich.apply(delta(Side::Bid, 47, 20));
  cheap.apply(delta(Side::Ask, 46, 70));
  // Gross sweep says 40c. The touch fill nets 20c - fee(10, 48) = +2c and
  // is taken; the deeper fill nets 20c - fee(20, 47) = -15c and is skipped.
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), 40);
  const auto fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(fill.net_cents, 2);
  EXPECT_EQ(fill.contracts, 10);
}

TEST(CrossedSweepNet, PricesTheFeeOnTheKalshiLeg) {
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 90, 10));
  cheap.apply(delta(Side::Ask, 85, 10));
  // Same books, opposite legs: gross is 50c either way, but the fee is
  // priced where Kalshi executes - fee(10, 90) = 7c against the rich bid,
  // fee(10, 85) = 9c against the cheap ask.
  const auto kalshi_rich = crossed_sweep_net(rich, cheap, true);
  EXPECT_EQ(kalshi_rich.net_cents, 43);
  const auto kalshi_cheap = crossed_sweep_net(rich, cheap, false);
  EXPECT_EQ(kalshi_cheap.net_cents, 41);
  EXPECT_EQ(kalshi_rich.contracts, 10);
  EXPECT_EQ(kalshi_cheap.contracts, 10);
}

TEST(CrossedSweep, SaturatesOnCorruptSizes) {
  // Level sizes saturate at int64 max on apply; the sweep math must
  // survive being handed them instead of overflowing (UB). The gross
  // sweep pins at the saturation ceiling; the net sweep stays a valid
  // non-negative fill because the (capped) fee comes off the ceiling.
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  OrderBook rich, cheap;
  rich.apply(delta(Side::Bid, 60, kMax));
  cheap.apply(delta(Side::Ask, 40, kMax));
  EXPECT_EQ(crossed_sweep_cents(rich, cheap), kMax);
  const auto fill = crossed_sweep_net(rich, cheap, true);
  EXPECT_GT(fill.net_cents, 0);
  EXPECT_LE(fill.net_cents, kMax);
  EXPECT_EQ(fill.contracts, kMax);
}
