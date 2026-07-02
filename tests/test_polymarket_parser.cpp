#include <gtest/gtest.h>

#include "feed/polymarket_parser.h"
#include "model/order_book.h"

using basis::feed::ParseStatus;
using basis::feed::PolymarketParser;
using basis::model::Action;
using basis::model::OrderBook;
using basis::model::Side;
using basis::model::Venue;

namespace {

constexpr std::string_view kBook = R"({
  "event_type": "book",
  "asset_id": "7132107",
  "market": "0xabc123",
  "bids": [{"price": "0.44", "size": "1200.5"}, {"price": "0.43", "size": "300"}],
  "asks": [{"price": "0.46", "size": "800"}],
  "timestamp": "1750000000000",
  "hash": "deadbeef"
})";

}  // namespace

TEST(PolymarketParser, BookSnapshotClearsThenSetsCentPrices) {
  PolymarketParser p;
  const auto r = p.parse(kBook, 2000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 4u);  // clear + 2 bids + 1 ask
  EXPECT_EQ(r.deltas[0].action, Action::Clear);
  EXPECT_EQ(r.deltas[0].venue, Venue::Polymarket);
  EXPECT_EQ(r.deltas[0].market, "7132107");  // keyed by asset_id
  EXPECT_EQ(r.deltas[0].ts_ns, 2000);

  OrderBook book;
  for (const auto& d : r.deltas) book.apply(d);
  EXPECT_EQ(*book.best_bid(), 44);  // "0.44" -> 44c
  EXPECT_EQ(*book.best_ask(), 46);
}

TEST(PolymarketParser, PriceChangeSetsAbsoluteSizeAndMapsSides) {
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "price_change",
    "market": "0xabc123",
    "price_changes": [
      {"asset_id": "7132107", "price": "0.44", "size": "0", "side": "BUY"},
      {"asset_id": "7132107", "price": "0.47", "size": "50", "side": "SELL"}
    ],
    "timestamp": "1750000000500"
  })", 3000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 2u);
  EXPECT_EQ(r.deltas[0].action, Action::Set);
  EXPECT_EQ(r.deltas[0].side, Side::Bid);
  EXPECT_EQ(r.deltas[0].size, 0);  // "0" removes the level
  EXPECT_EQ(r.deltas[1].side, Side::Ask);
  EXPECT_EQ(r.deltas[1].price_cents, 47);
  EXPECT_EQ(r.deltas[1].size, 50);
}

TEST(PolymarketParser, TopLevelArrayOfEventsIsHandled) {
  PolymarketParser p;
  std::string payload = "[";
  payload += kBook;
  payload += R"(, {"event_type": "tick_size_change", "asset_id": "7132107"}])";
  const auto r = p.parse(payload, 0);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.deltas.size(), 4u);  // book expands, tick_size_change ignored
}

TEST(PolymarketParser, DecimalPricesRoundHalfUpToCents) {
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "price_change",
    "market": "0xabc123",
    "price_changes": [
      {"asset_id": "t", "price": "0.475", "size": "10", "side": "BUY"},
      {"asset_id": "t", "price": "0.474", "size": "10", "side": "BUY"},
      {"asset_id": "t", "price": "0.005", "size": "10", "side": "BUY"}
    ]
  })", 0);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 3u);
  EXPECT_EQ(r.deltas[0].price_cents, 48);  // 0.475 rounds up
  EXPECT_EQ(r.deltas[1].price_cents, 47);  // 0.474 rounds down
  EXPECT_EQ(r.deltas[2].price_cents, 1);   // 0.005 rounds up, stays a level
}

TEST(PolymarketParser, TinyFractionalSizeDoesNotVanish) {
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "price_change",
    "market": "0xabc123",
    "price_changes": [
      {"asset_id": "t", "price": "0.44", "size": "0.4", "side": "BUY"}
    ]
  })", 0);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  // A real resting 0.4-share level must not round to "remove this level".
  EXPECT_EQ(r.deltas[0].size, 1);
}

TEST(PolymarketParser, IgnoresUnknownEventsAndFlagsMalformed) {
  PolymarketParser p;
  EXPECT_EQ(p.parse(R"({"event_type": "last_trade_price"})", 0).status,
            ParseStatus::Ignored);
  EXPECT_EQ(p.parse("{{{", 0).status, ParseStatus::Malformed);
  const auto r = p.parse(R"({
    "event_type": "book", "asset_id": "t",
    "bids": [{"price": "abc", "size": "1"}]
  })", 0);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
}
