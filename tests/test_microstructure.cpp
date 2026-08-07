#include <gtest/gtest.h>

#include "analytics/microstructure.h"
#include "model/book_delta.h"

using basis::analytics::microprice_cents;
using basis::analytics::queue_imbalance;
using basis::model::Action;
using basis::model::OrderBook;
using basis::model::Side;

namespace {

void set_level(OrderBook& b, Side side, int price, std::int64_t size) {
  basis::model::BookDelta d;
  d.action = Action::Set;
  d.side = side;
  d.price_cents = price;
  d.size = size;
  b.apply(d);
}

}  // namespace

TEST(Microstructure, BalancedBookPutsMicropriceAtTheMid) {
  OrderBook b;
  set_level(b, Side::Bid, 40, 100);
  set_level(b, Side::Ask, 60, 100);
  ASSERT_TRUE(microprice_cents(b).has_value());
  EXPECT_DOUBLE_EQ(*microprice_cents(b), 50.0);  // same as the mid
  EXPECT_DOUBLE_EQ(*queue_imbalance(b), 0.0);
}

TEST(Microstructure, HeavyBidPullsPriceTowardTheAsk) {
  OrderBook b;
  set_level(b, Side::Bid, 40, 900);  // buyers lined up
  set_level(b, Side::Ask, 60, 100);
  // (40*100 + 60*900) / 1000 = 58: nine tenths of the way to the offer,
  // because the next trade is far likelier to lift it than to hit the bid.
  EXPECT_DOUBLE_EQ(*microprice_cents(b), 58.0);
  EXPECT_DOUBLE_EQ(*queue_imbalance(b), 0.8);
  // The mid says 50 regardless, which is the bias this measures.
  EXPECT_DOUBLE_EQ(*b.mid(), 50.0);
}

TEST(Microstructure, HeavyAskPullsPriceTowardTheBid) {
  OrderBook b;
  set_level(b, Side::Bid, 40, 100);
  set_level(b, Side::Ask, 60, 900);
  EXPECT_DOUBLE_EQ(*microprice_cents(b), 42.0);
  EXPECT_DOUBLE_EQ(*queue_imbalance(b), -0.8);
}

TEST(Microstructure, ImbalanceStaysInRangeAtTheExtremes) {
  OrderBook b;
  set_level(b, Side::Bid, 40, 1);
  set_level(b, Side::Ask, 60, 1'000'000);
  const double imb = *queue_imbalance(b);
  EXPECT_GT(imb, -1.0);
  EXPECT_LT(imb, 0.0);
  // Microprice never escapes the touch, however lopsided the queues get.
  const double micro = *microprice_cents(b);
  EXPECT_GE(micro, 40.0);
  EXPECT_LE(micro, 60.0);
}

TEST(Microstructure, OneSidedBookHasNeither) {
  OrderBook b;
  set_level(b, Side::Bid, 40, 100);
  EXPECT_FALSE(microprice_cents(b).has_value());
  EXPECT_FALSE(queue_imbalance(b).has_value());
  OrderBook empty;
  EXPECT_FALSE(microprice_cents(empty).has_value());
  EXPECT_FALSE(queue_imbalance(empty).has_value());
}
