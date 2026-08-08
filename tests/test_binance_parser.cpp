#include <gtest/gtest.h>

#include "feed/binance_parser.h"
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
  ASSERT_EQ(r.deltas.size(), 2u);
  EXPECT_EQ(r.deltas[0].venue, Venue::Binance);
  EXPECT_EQ(r.deltas[0].market, "BNBUSDT");
  EXPECT_EQ(r.deltas[0].side, Side::Bid);
  EXPECT_EQ(r.deltas[0].action, Action::Set);
  EXPECT_EQ(r.deltas[0].price_cents, 60255);          // 602.55 exactly
  EXPECT_EQ(r.deltas[0].size, 460'000'000);           // 4.6 scaled by 1e8
  EXPECT_EQ(r.deltas[1].side, Side::Ask);
  EXPECT_EQ(r.deltas[1].price_cents, 60256);
  EXPECT_EQ(r.deltas[1].ts_ns, 1000);
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
  EXPECT_EQ(r.deltas[0].price_cents, 7);
  EXPECT_EQ(r.deltas[1].price_cents, 8);
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
