#include <gtest/gtest.h>

#include "feed/coinbase_parser.h"
#include "model/order_book.h"
#include "model/types.h"

using basis::feed::CoinbaseParser;
using basis::feed::ParseStatus;
using basis::model::Action;
using basis::model::Aggressor;
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

// ----- prints ---------------------------------------------------------------
//
// A ticker frame is both a trade and a touch update. The engine read only
// the touch, which is what made it a book engine rather than a
// market-data one: it carried what the book would do and never what it
// did.

TEST(CoinbaseParser, TickerCarriesAPrintAsWellAsTheTouch) {
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"ticker","sequence":134071694194,"product_id":"BTC-USD",)"
      R"("price":"63748.91","last_size":"0.00312","side":"sell",)"
      R"("trade_id":712345678,)"
      R"("best_bid":"63748.90","best_bid_size":"0.05",)"
      R"("best_ask":"63748.91","best_ask_size":"0.01"})", 1000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  // Both halves, from one message.
  EXPECT_EQ(r.deltas.size(), 2u);
  ASSERT_EQ(r.trades.size(), 1u);
  const auto& t = r.trades[0];
  EXPECT_EQ(t.venue, Venue::Coinbase);
  EXPECT_EQ(t.market, "BTC-USD");
  EXPECT_EQ(t.price_cents, 6'374'891);
  EXPECT_EQ(t.size, 312'000);          // 0.00312 scaled by 1e8
  EXPECT_EQ(t.trade_id, 712345678u);
  EXPECT_EQ(t.ts_ns, 1000);
}

TEST(CoinbaseParser, TheAggressorIsTheOppositeOfTheSideCoinbaseReports) {
  // Coinbase reports the MAKER's side. "sell" means a resting sell was
  // hit, so the taker was a buyer. Getting this backwards flips the sign
  // of any order-flow measure built on it, and both values look plausible
  // in the data, so it is worth its own check rather than a comment.
  CoinbaseParser p;
  const auto hit_a_sell = p.parse(
      R"({"type":"match","product_id":"BTC-USD","price":"63748.91",)"
      R"("size":"0.01","side":"sell","trade_id":1})", 1);
  ASSERT_EQ(hit_a_sell.trades.size(), 1u);
  EXPECT_EQ(hit_a_sell.trades[0].aggressor, Aggressor::Buy);

  CoinbaseParser q;
  const auto hit_a_buy = q.parse(
      R"({"type":"match","product_id":"BTC-USD","price":"63748.91",)"
      R"("size":"0.01","side":"buy","trade_id":2})", 1);
  ASSERT_EQ(hit_a_buy.trades.size(), 1u);
  EXPECT_EQ(hit_a_buy.trades[0].aggressor, Aggressor::Sell);
}

TEST(CoinbaseParser, AVenueThatDoesNotSayWhoAggressedLeavesItUnknown) {
  // Unknown is a real state, not a default to tidy away. A guessed
  // aggressor corrupts order-flow imbalance silently; an honest Unknown
  // lets a consumer report coverage.
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"match","product_id":"BTC-USD","price":"63748.91",)"
      R"("size":"0.01","trade_id":3})", 1);
  ASSERT_EQ(r.trades.size(), 1u);
  EXPECT_EQ(r.trades[0].aggressor, Aggressor::Unknown);
}

TEST(CoinbaseParser, AMatchIsAPrintAndNotABookUpdate) {
  // The match channel carries no book fields. A trade is an event at a
  // price, not a change to a level, and it must not reach the book.
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"match","trade_id":9,"product_id":"BTC-USD",)"
      R"("price":"63748.91","size":"0.02","side":"buy"})", 7);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_TRUE(r.deltas.empty());
  ASSERT_EQ(r.trades.size(), 1u);
  EXPECT_EQ(r.trades[0].size, 2'000'000);
}

TEST(CoinbaseParser, ATickerWithNoTouchStillYieldsItsPrint) {
  // The book fields and the print are independent halves of the frame.
  // Requiring the touch to be present threw the trade away with it.
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"ticker","product_id":"BTC-USD","price":"63748.91",)"
      R"("last_size":"0.5","side":"buy","trade_id":11})", 3);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_TRUE(r.deltas.empty());
  ASSERT_EQ(r.trades.size(), 1u);
  EXPECT_EQ(r.trades[0].price_cents, 6'374'891);
}

TEST(CoinbaseParser, ATickerForSomethingThatHasNeverTradedIsStillIgnored) {
  // No book, no print: an absence of data, not a broken message. This is
  // the case the print path must not turn into a spurious trade.
  CoinbaseParser p;
  const auto r = p.parse(
      R"({"type":"ticker","product_id":"BTC-USD"})", 3);
  EXPECT_EQ(r.status, ParseStatus::Ignored);
  EXPECT_TRUE(r.deltas.empty());
  EXPECT_TRUE(r.trades.empty());
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
