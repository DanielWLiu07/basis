#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

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

TEST(PolymarketParser, DigitlessAndTrailingGarbagePricesAreMalformed) {
  PolymarketParser p;
  // A field with a decimal point but no digit ("." ) must not slip through
  // as a phantom level at price 0.
  EXPECT_EQ(p.parse(R"({"event_type":"book","asset_id":"t",)"
                    R"("bids":[{"price":".","size":"5"}]})", 0)
                .status,
            ParseStatus::Malformed);
  // Trailing garbage after the last significant digit is corruption, not a
  // truncatable price.
  EXPECT_EQ(p.parse(R"({"event_type":"book","asset_id":"t",)"
                    R"("bids":[{"price":"0.505junk","size":"5"}]})", 0)
                .status,
            ParseStatus::Malformed);
  // Legitimate spellings of zero and plain values still parse.
  EXPECT_EQ(p.parse(R"({"event_type":"book","asset_id":"t",)"
                    R"("bids":[{"price":"0.","size":"5"}],)"
                    R"("asks":[{"price":"0.47","size":"5"}]})", 0)
                .status,
            ParseStatus::Ok);
}

TEST(PolymarketParser, HugeOrOutOfRangeNumbersAreMalformedNotUB) {
  PolymarketParser p;
  // Digit-cap: this would overflow int64 accumulation.
  EXPECT_EQ(p.parse(R"({
    "event_type": "price_change", "market": "0x1",
    "price_changes": [{"asset_id": "t",
      "price": "99999999999999999999999999", "size": "1", "side": "BUY"}]
  })", 0).status, ParseStatus::Malformed);
  // Fits int64 but is not a probability: must not wrap into a fake level.
  EXPECT_EQ(p.parse(R"({
    "event_type": "price_change", "market": "0x1",
    "price_changes": [{"asset_id": "t",
      "price": "40000000", "size": "1", "side": "BUY"}]
  })", 0).status, ParseStatus::Malformed);
}

TEST(PolymarketParser, BoundaryPricesZeroAndOneAreAccepted) {
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "price_change", "market": "0x1",
    "price_changes": [
      {"asset_id": "t", "price": "0.00", "size": "10", "side": "BUY"},
      {"asset_id": "t", "price": "1.00", "size": "10", "side": "SELL"}
    ]
  })", 0);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.deltas[0].price_cents, 0);
  EXPECT_EQ(r.deltas[1].price_cents, 100);
}

TEST(PolymarketParser, WrongTypedBookSideIsMalformedNotEmpty) {
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "book", "asset_id": "t",
    "bids": "oops",
    "asks": [{"price": "0.46", "size": "1"}]
  })", 0);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
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

namespace {

std::string load_fixture(const char* name) {
  std::ifstream in(std::string(BASIS_SOURCE_DIR "/tests/data/") + name);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

// The fixture is a real captured wire message with the hash the venue
// actually sent; the parser must recompute and match it. Tampering with
// one size must flip the verdict: a corrupted book cannot verify.
TEST(PolymarketParser, VerifiesARealSnapshotHash) {
  PolymarketParser p;
  const auto raw = load_fixture("polymarket_book_snapshot.json");
  ASSERT_FALSE(raw.empty());

  const auto r = p.parse(raw, 0);
  EXPECT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.hashes_verified, 1u);
  EXPECT_EQ(r.hashes_mismatched, 0u);

  auto tampered = raw;
  const auto pos = tampered.find("2341150.69");
  ASSERT_NE(pos, std::string::npos);
  tampered.replace(pos, 10, "2341150.70");
  EXPECT_EQ(p.parse(tampered, 0).hashes_mismatched, 1u);
}

TEST(PolymarketParser, RefreshFormSnapshotsAreUnverifiable) {
  // The venue's periodic book refreshes omit tick_size and
  // last_trade_price, which its hash covers; those are counted, not
  // guessed at.
  PolymarketParser p;
  const auto r = p.parse(R"({
    "event_type": "book", "asset_id": "t", "market": "0xabc",
    "timestamp": "1", "hash": "0000000000000000000000000000000000000000",
    "bids": [{"price": "0.45", "size": "10"}], "asks": []
  })", 0);
  EXPECT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.hashes_unverifiable, 1u);

  // No hash at all: nothing to check, so nothing is claimed.
  const auto r2 = p.parse(R"({
    "event_type": "book", "asset_id": "t",
    "bids": [{"price": "0.45", "size": "10"}], "asks": []
  })", 0);
  EXPECT_EQ(r2.hashes_verified + r2.hashes_mismatched +
                r2.hashes_unverifiable,
            0u);
}
