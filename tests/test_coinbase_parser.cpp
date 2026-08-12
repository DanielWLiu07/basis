#include <gtest/gtest.h>

#include "feed/coinbase_parser.h"
#include "model/order_book.h"
#include "model/types.h"

using basis::feed::CoinbaseParser;
using basis::feed::ParseStatus;
using basis::model::Action;
using basis::model::OrderBook;
using basis::model::Side;
using basis::model::Venue;

// Every payload below is a real message shape from a live BTC-USD capture,
// not a hand-written approximation of the wire format.

TEST(CoinbaseParser, TickerBecomesBothSidesOfTheTouch) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"ticker","sequence":134071694194,"product_id":"BTC-USD",)"
      R"("price":"63748.91","best_bid":"63748.90","best_bid_size":"0.05",)"
      R"("best_ask":"63748.91","best_ask_size":"0.01"})", 1000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 2u);
  EXPECT_EQ(r.deltas[0].venue, Venue::Coinbase);
  EXPECT_EQ(r.deltas[0].market, "BTC-USD");
  EXPECT_EQ(r.deltas[0].side, Side::Bid);
  EXPECT_EQ(r.deltas[0].action, Action::Set);
  EXPECT_EQ(r.deltas[0].price_cents, 6'374'890);  // 63748.90 exactly
  EXPECT_EQ(r.deltas[0].size, 5'000'000);         // 0.05 scaled by 1e8
  EXPECT_EQ(r.deltas[1].side, Side::Ask);
  EXPECT_EQ(r.deltas[1].price_cents, 6'374'891);
  EXPECT_EQ(r.deltas[1].ts_ns, 1000);
}

TEST(CoinbaseParser, SnapshotLeadsWithClearSoAReconnectRebuildsTheBook) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"snapshot","product_id":"BTC-USD",)"
      R"("bids":[["63748.90","0.05"],["63748.00","1.25"]],)"
      R"("asks":[["63749.10","0.30"]]})", 5);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 4u);
  // Without the Clear, a reconnect's snapshot would layer over stale levels
  // the new image happens not to mention.
  EXPECT_EQ(r.deltas[0].action, Action::Clear);
  EXPECT_EQ(r.deltas[1].side, Side::Bid);
  EXPECT_EQ(r.deltas[1].price_cents, 6'374'890);
  EXPECT_EQ(r.deltas[3].side, Side::Ask);
  EXPECT_EQ(r.deltas[3].price_cents, 6'374'910);
}

TEST(CoinbaseParser, L2UpdateSetsLevelsAndZeroRemovesThem) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"l2update","product_id":"BTC-USD","changes":)"
      R"([["buy","63748.90","0.07"],["sell","63749.10","0"]]})", 7);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 2u);
  EXPECT_EQ(r.deltas[0].side, Side::Bid);
  EXPECT_EQ(r.deltas[0].size, 7'000'000);
  EXPECT_EQ(r.deltas[1].side, Side::Ask);
  EXPECT_EQ(r.deltas[1].action, Action::Set);
  EXPECT_EQ(r.deltas[1].size, 0);  // Set to zero is the venue's removal
}

// The regression this whole distinction exists for. A real BTC-USD snapshot
// carries resting asks above $20,000,000, which cannot fit in int32 cents.
// Rejecting the message over them threw away the entire 45,177-level book
// image and left every later diff applying to an empty book, which produced
// a confident and completely wrong lead-lag result.
TEST(CoinbaseParser, UnrepresentableDeepLevelIsCountedNotFatal) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"snapshot","product_id":"BTC-USD",)"
      R"("bids":[["63748.90","0.05"]],)"
      R"("asks":[["63749.10","0.30"],["138991023.41","0.00021"]]})", 9);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.levels_unrepresentable, 1u);
  // Clear + one bid + the one representable ask.
  ASSERT_EQ(r.deltas.size(), 3u);
  EXPECT_EQ(r.deltas[2].price_cents, 6'374'910);

  // The levels that survive are the ones that can ever be top of book, so
  // the touch is intact despite the drop.
  OrderBook book;
  for (const auto& d : r.deltas) book.apply(d);
  ASSERT_TRUE(book.best_bid().has_value());
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(*book.best_bid(), 6'374'890);
  EXPECT_EQ(*book.best_ask(), 6'374'910);
}

TEST(CoinbaseParser, OffGridPriceIsRejectedRatherThanTruncated) {
  CoinbaseParser p;
  // A price finer than a cent has no exact representation here; rounding it
  // would put a book in front of the analytics that the venue never quoted.
  const auto r = p.parse(
      R"({"type":"l2update","product_id":"BTC-USD","changes":)"
      R"([["buy","63748.905","0.07"]]})", 11);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
}

TEST(CoinbaseParser, ControlFramesAreIgnoredNotMalformed) {
  CoinbaseParser p;
  for (const char* payload : {
           R"({"type":"subscriptions","channels":[{"name":"ticker"}]})",
           R"({"type":"heartbeat","product_id":"BTC-USD","sequence":1})",
           R"({"type":"match","product_id":"BTC-USD","size":"0.01"})",
       }) {
    const auto r = p.parse(payload, 1);
    EXPECT_EQ(r.status, ParseStatus::Ignored) << payload;
  }
}

TEST(CoinbaseParser, BrokenPayloadsAreMalformed) {
  CoinbaseParser p;
  EXPECT_EQ(p.parse("{not json", 1).status, ParseStatus::Malformed);
  // A change triple missing its size, and an unknown side.
  EXPECT_EQ(p.parse(
      R"({"type":"l2update","product_id":"BTC-USD",)"
      R"("changes":[["buy","63748.90"]]})", 1).status,
      ParseStatus::Malformed);
  EXPECT_EQ(p.parse(
      R"({"type":"l2update","product_id":"BTC-USD",)"
      R"("changes":[["sideways","63748.90","0.1"]]})", 1).status,
      ParseStatus::Malformed);
}

// Prices from the two venues must land on the same scale or the basis
// between them would be an artifact of the parsers rather than the market.
TEST(CoinbaseParser, SharesTheBinanceScaleSoMidsAreComparable) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"ticker","product_id":"BTC-USD","best_bid":"63748.90",)"
      R"("best_bid_size":"1.0","best_ask":"63749.10","best_ask_size":"1.0"})",
      1);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  OrderBook book;
  for (const auto& d : r.deltas) book.apply(d);
  ASSERT_TRUE(book.mid().has_value());
  EXPECT_DOUBLE_EQ(*book.mid(), 6'374'900.0);  // $63,749.00 in cents
  EXPECT_EQ(r.deltas[0].size, 100'000'000);    // 1.0 scaled by 1e8
}
