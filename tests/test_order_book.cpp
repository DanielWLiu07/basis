#include <gtest/gtest.h>

#include "model/order_book.h"

using basis::model::BookDelta;
using basis::model::OrderBook;
using basis::model::Side;
using basis::model::Venue;

namespace {

BookDelta delta(Side side, int price, std::int64_t size, std::uint64_t seq = 0) {
  return BookDelta{Venue::Kalshi, "TEST", side, price, size, seq, 0};
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

TEST(OrderBook, ClearEmptiesBook) {
  OrderBook b;
  b.apply(delta(Side::Bid, 45, 100));
  b.apply(delta(Side::Ask, 55, 100));
  b.clear();
  EXPECT_TRUE(b.empty());
}
