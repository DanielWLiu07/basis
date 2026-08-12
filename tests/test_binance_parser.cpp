#include <gtest/gtest.h>

#include "feed/binance_parser.h"
#include "model/order_book.h"
#include "model/types.h"

using basis::feed::BinanceParser;
using basis::feed::ParseStatus;
using basis::model::Action;
using basis::model::Side;
using basis::model::Venue;

// Every payload below is a real message from the committed capture, not a
// hand-written approximation of the wire format.

TEST(BinanceParser, BookTickerBecomesBothSidesOfTheTouch) {
  BinanceParser p;
  const auto r = p.parse(
      R"({"stream":"bnbusdt@bookTicker","data":{"u":718455119,"s":"BNBUSDT",)"
      R"("b":"602.55000000","B":"4.60000000",)"
      R"("a":"602.56000000","A":"33.80000000"}})", 1000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  // Clear, then the two sides: bookTicker replaces the touch rather than
  // adding to it (see BookTickerReplacesTheTouchInsteadOfAccumulating).
  ASSERT_EQ(r.deltas.size(), 3u);
  EXPECT_EQ(r.deltas[0].action, Action::Clear);
  EXPECT_EQ(r.deltas[1].venue, Venue::Binance);
  EXPECT_EQ(r.deltas[1].market, "BNBUSDT");
  EXPECT_EQ(r.deltas[1].side, Side::Bid);
  EXPECT_EQ(r.deltas[1].action, Action::Set);
  EXPECT_EQ(r.deltas[1].price_cents, 60255);          // 602.55 exactly
  EXPECT_EQ(r.deltas[1].size, 460'000'000);           // 4.6 scaled by 1e8
  EXPECT_EQ(r.deltas[2].side, Side::Ask);
  EXPECT_EQ(r.deltas[2].price_cents, 60256);
  EXPECT_EQ(r.deltas[2].ts_ns, 1000);
}

TEST(BinanceParser, DepthUpdateCarriesEveryLevelAndZeroRemoves) {
  BinanceParser p;
  const auto r = p.parse(
      R"({"stream":"bnbusdt@depth@100ms","data":{"e":"depthUpdate",)"
      R"("E":1786225029606,"s":"BNBUSDT","U":20307736409,"u":20307736409,)"
      R"("b":[["602.50000000","0.00000000"]],)"
      R"("a":[["602.55000000","1.48200000"],["602.60000000","3.00000000"]]}})",
      2000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 3u);
  // Zero size is the venue's level removal, and Action::Set carries it.
  EXPECT_EQ(r.deltas[0].side, Side::Bid);
  EXPECT_EQ(r.deltas[0].price_cents, 60250);
  EXPECT_EQ(r.deltas[0].size, 0);
  EXPECT_EQ(r.deltas[1].side, Side::Ask);
  EXPECT_EQ(r.deltas[1].size, 148'200'000);
  EXPECT_EQ(r.deltas[2].price_cents, 60260);
}

TEST(BinanceParser, PriceOffTheCentGridIsRejectedNotTruncated) {
  // ALLOUSDT really quotes 0.32430000, which is 32.43 cents: not
  // representable in this engine's integer-cent price model. Truncating
  // to 32 would put a wrong book in front of the analytics, so the
  // message is malformed instead, and nothing is emitted.
  BinanceParser p;
  const auto r = p.parse(
      R"({"stream":"allousdt@bookTicker","data":{"u":718455119,)"
      R"("s":"ALLOUSDT","b":"0.32430000","B":"460.00000000",)"
      R"("a":"0.32440000","A":"33.80000000"}})", 3000);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
}

TEST(BinanceParser, ExactSubDollarPricesStillWork) {
  // The rule is the cent grid, not a minimum price: 0.07 is exact.
  BinanceParser p;
  const auto r = p.parse(
      R"({"stream":"x@bookTicker","data":{"s":"XUSDT",)"
      R"("b":"0.07000000","B":"1.00000000",)"
      R"("a":"0.08000000","A":"1.00000000"}})", 4000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  ASSERT_EQ(r.deltas.size(), 3u);  // Clear + both sides
  EXPECT_EQ(r.deltas[1].price_cents, 7);
  EXPECT_EQ(r.deltas[2].price_cents, 8);
}

TEST(BinanceParser, ControlFramesAndTradesAreIgnoredNotMalformed) {
  BinanceParser p;
  // Subscription ack: no symbol at all.
  EXPECT_EQ(p.parse(R"({"result":null,"id":1})", 1).status,
            ParseStatus::Ignored);
  // A trade has a symbol but no book fields; it is valid, just not a book
  // update, and must not be counted against the integrity numbers.
  EXPECT_EQ(p.parse(
      R"({"stream":"btcusdt@trade","data":{"e":"trade","s":"BTCUSDT",)"
      R"("p":"64000.00000000","q":"0.01000000"}})", 2).status,
            ParseStatus::Ignored);
}

TEST(BinanceParser, StructurallyBrokenPayloadsAreMalformed) {
  BinanceParser p;
  EXPECT_EQ(p.parse("{not json", 1).status, ParseStatus::Malformed);
  EXPECT_EQ(p.parse(R"({"stream":"x@depth","data":{"e":"depthUpdate",)"
                    R"("s":"XUSDT","b":[["notaprice","1.0"]],"a":[]}})", 1)
                .status,
            ParseStatus::Malformed);
}

// bookTicker replaces the touch rather than diffing it. Applying it as bare
// Set deltas left the previous best bid and ask resting in the book, so the
// touch decayed into the running max bid and min ask -- on a real 40-minute
// capture, 204 phantom levels and a spread of -$54.87 by message 20,000.
// Throughput benchmarks, the only consumer at the time, could not see it.
TEST(BinanceParser, BookTickerReplacesTheTouchInsteadOfAccumulating) {
  BinanceParser p;
  basis::model::OrderBook book;
  const auto apply = [&](const char* payload) {
    const auto r = p.parse(payload, 1);
    EXPECT_EQ(r.status, ParseStatus::Ok);
    for (const auto& d : r.deltas) book.apply(d);
  };
  apply(R"({"s":"BTCUSDT","b":"63842.10","B":"1.0",)"
        R"("a":"63842.11","A":"1.0"})");
  // The whole touch moves down; the earlier quotes must not survive it.
  apply(R"({"s":"BTCUSDT","b":"63790.00","B":"2.0",)"
        R"("a":"63790.01","A":"2.0"})");

  ASSERT_TRUE(book.best_bid().has_value());
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(*book.best_bid(), 6'379'000);
  EXPECT_EQ(*book.best_ask(), 6'379'001);
  // The bug's signature: a crossed book, best_ask below best_bid.
  EXPECT_LT(*book.best_bid(), *book.best_ask());
}

// The diff stream is a genuine per-level update and must NOT be cleared,
// or every depthUpdate would throw away the rest of the book.
TEST(BinanceParser, DepthUpdateDoesNotClearTheBook) {
  BinanceParser p;
  basis::model::OrderBook book;
  const auto apply = [&](const char* payload) {
    const auto r = p.parse(payload, 1);
    EXPECT_EQ(r.status, ParseStatus::Ok);
    for (const auto& d : r.deltas) book.apply(d);
  };
  apply(R"({"e":"depthUpdate","s":"BTCUSDT",)"
        R"("b":[["63790.00","2.0"],["63789.00","1.0"]],)"
        R"("a":[["63790.01","2.0"]]})");
  // A later update touching only one level leaves the others standing.
  apply(R"({"e":"depthUpdate","s":"BTCUSDT",)"
        R"("b":[["63790.00","0"]],"a":[]})");
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(*book.best_bid(), 6'378'900);  // the deeper bid survived
}
