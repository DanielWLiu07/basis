#include <gtest/gtest.h>

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
