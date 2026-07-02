#include <gtest/gtest.h>

#include "model/unified_book.h"

using basis::model::BookDelta;
using basis::model::Side;
using basis::model::UnifiedBook;
using basis::model::Venue;

namespace {

BookDelta delta(Venue venue, Side side, int price, std::int64_t size) {
  return BookDelta{.venue = venue,
                   .market = "TEST",
                   .side = side,
                   .price_cents = price,
                   .size = size};
}

}  // namespace

TEST(UnifiedBook, RoutesDeltasByVenue) {
  UnifiedBook u;
  u.apply(delta(Venue::Kalshi, Side::Bid, 47, 100));
  u.apply(delta(Venue::Polymarket, Side::Bid, 44, 100));
  EXPECT_EQ(*u.book(Venue::Kalshi).best_bid(), 47);
  EXPECT_EQ(*u.book(Venue::Polymarket).best_bid(), 44);
  EXPECT_FALSE(u.book(Venue::Kalshi).best_ask().has_value());
}

TEST(UnifiedBook, BasisNeedsBothMids) {
  UnifiedBook u;
  u.apply(delta(Venue::Kalshi, Side::Bid, 47, 100));
  u.apply(delta(Venue::Kalshi, Side::Ask, 49, 100));
  EXPECT_FALSE(u.basis().has_value());  // polymarket book still empty
  u.apply(delta(Venue::Polymarket, Side::Bid, 44, 100));
  u.apply(delta(Venue::Polymarket, Side::Ask, 46, 100));
  ASSERT_TRUE(u.basis().has_value());
  EXPECT_DOUBLE_EQ(*u.basis(), 48.0 - 45.0);
}

TEST(UnifiedBook, BasisSignFollowsKalshiMinusPolymarket) {
  UnifiedBook u;
  u.apply(delta(Venue::Kalshi, Side::Bid, 40, 100));
  u.apply(delta(Venue::Kalshi, Side::Ask, 42, 100));
  u.apply(delta(Venue::Polymarket, Side::Bid, 44, 100));
  u.apply(delta(Venue::Polymarket, Side::Ask, 46, 100));
  EXPECT_DOUBLE_EQ(*u.basis(), 41.0 - 45.0);  // negative: polymarket richer
}
