#include <gtest/gtest.h>

#include "feed/kalshi_parser.h"
#include "model/order_book.h"

using basis::feed::KalshiParser;
using basis::feed::ParseStatus;
using basis::model::Action;
using basis::model::OrderBook;
using basis::model::Side;
using basis::model::Venue;

namespace {

constexpr std::string_view kSnapshot = R"({
  "type": "orderbook_snapshot", "sid": 1, "seq": 10,
  "msg": {
    "market_ticker": "FED-26SEP-CUT",
    "yes": [[45, 100], [44, 250]],
    "no":  [[53, 80]]
  }
})";

}  // namespace

TEST(KalshiParser, SnapshotClearsThenSetsBothSides) {
  KalshiParser p;
  const auto r = p.parse(kSnapshot, 1000);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_FALSE(r.gap);
  ASSERT_EQ(r.deltas.size(), 4u);  // clear + 2 yes + 1 no
  EXPECT_EQ(r.deltas[0].action, Action::Clear);
  EXPECT_EQ(r.deltas[0].market, "FED-26SEP-CUT");
  EXPECT_EQ(r.deltas[0].venue, Venue::Kalshi);
  EXPECT_EQ(r.deltas[0].ts_ns, 1000);

  OrderBook book;
  for (const auto& d : r.deltas) book.apply(d);
  EXPECT_EQ(*book.best_bid(), 45);
  // NO bid at 53 folds into a YES ask at 100 - 53 = 47.
  EXPECT_EQ(*book.best_ask(), 47);
}

TEST(KalshiParser, DeltaIsRelativeAndFoldsNoSide) {
  KalshiParser p;
  ASSERT_EQ(p.parse(kSnapshot, 0).status, ParseStatus::Ok);

  const auto r = p.parse(R"({
    "type": "orderbook_delta", "sid": 1, "seq": 11,
    "msg": { "market_ticker": "FED-26SEP-CUT",
             "price": 53, "delta": -80, "side": "no" }
  })", 0);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_FALSE(r.gap);
  ASSERT_EQ(r.deltas.size(), 1u);
  EXPECT_EQ(r.deltas[0].action, Action::Add);
  EXPECT_EQ(r.deltas[0].side, Side::Ask);
  EXPECT_EQ(r.deltas[0].price_cents, 47);
  EXPECT_EQ(r.deltas[0].size, -80);
}

TEST(KalshiParser, SequenceGapClearsBookAndFlags) {
  KalshiParser p;
  ASSERT_EQ(p.parse(kSnapshot, 0).status, ParseStatus::Ok);  // seq 10

  const auto r = p.parse(R"({
    "type": "orderbook_delta", "sid": 1, "seq": 13,
    "msg": { "market_ticker": "FED-26SEP-CUT",
             "price": 45, "delta": 10, "side": "yes" }
  })", 0);  // seq 11 and 12 were missed
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_TRUE(r.gap);
  // The live feed unsubscribes this sid to force a fresh snapshot.
  EXPECT_EQ(r.sid, 1u);
  ASSERT_EQ(r.deltas.size(), 2u);
  EXPECT_EQ(r.deltas[0].action, Action::Clear);
  EXPECT_EQ(r.deltas[1].action, Action::Add);
}

TEST(KalshiParser, ContiguousSequenceHasNoGap) {
  KalshiParser p;
  ASSERT_EQ(p.parse(kSnapshot, 0).status, ParseStatus::Ok);  // seq 10
  const char* fmt = R"({
    "type": "orderbook_delta", "sid": 1, "seq": %d,
    "msg": { "market_ticker": "FED-26SEP-CUT",
             "price": 45, "delta": 1, "side": "yes" }
  })";
  for (int seq = 11; seq <= 13; ++seq) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, seq);
    const auto r = p.parse(buf, 0);
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_FALSE(r.gap) << "seq " << seq;
  }
}

TEST(KalshiParser, IgnoresCommandRepliesAndUnknownTypes) {
  KalshiParser p;
  EXPECT_EQ(p.parse(R"({"id": 2, "type": "subscribed"})", 0).status,
            ParseStatus::Ignored);
  EXPECT_EQ(p.parse(R"({"type": "ticker", "msg": {}})", 0).status,
            ParseStatus::Ignored);
}

TEST(KalshiParser, SequenceIsPerSubscriptionNotPerMarket) {
  // One subscription (sid) carries interleaved markets on a single shared
  // sequence; per-market tracking would flag a false gap on every delta.
  KalshiParser p;
  ASSERT_EQ(p.parse(R"({"type":"orderbook_snapshot","sid":1,"seq":10,
    "msg":{"market_ticker":"A","yes":[[40,10]],"no":[]}})", 0).status,
            ParseStatus::Ok);
  ASSERT_EQ(p.parse(R"({"type":"orderbook_snapshot","sid":1,"seq":11,
    "msg":{"market_ticker":"B","yes":[[60,10]],"no":[]}})", 0).status,
            ParseStatus::Ok);
  const char* fmt = R"({"type":"orderbook_delta","sid":1,"seq":%d,
    "msg":{"market_ticker":"%s","price":45,"delta":1,"side":"yes"}})";
  int seq = 12;
  for (const char* market : {"A", "B", "A", "B"}) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, seq++, market);
    const auto r = p.parse(buf, 0);
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_FALSE(r.gap) << "market " << market << " seq " << seq - 1;
  }
}

TEST(KalshiParser, DeltaBeforeSnapshotFlagsGapOnce) {
  KalshiParser p;
  // No snapshot was ever seen for this market: there is no book to apply
  // the delta to, so the stream is untrustworthy until a snapshot arrives.
  const auto first = p.parse(R"({
    "type": "orderbook_delta", "sid": 1, "seq": 5,
    "msg": { "market_ticker": "NEVER-SNAPSHOTTED",
             "price": 45, "delta": 10, "side": "yes" }
  })", 0);
  ASSERT_EQ(first.status, ParseStatus::Ok);
  EXPECT_TRUE(first.gap);
  EXPECT_EQ(first.deltas[0].action, Action::Clear);

  const auto second = p.parse(R"({
    "type": "orderbook_delta", "sid": 1, "seq": 6,
    "msg": { "market_ticker": "NEVER-SNAPSHOTTED",
             "price": 45, "delta": 10, "side": "yes" }
  })", 0);
  EXPECT_FALSE(second.gap);  // flagged once, not on every delta
}

TEST(KalshiParser, WrongTypedBookSideIsMalformedNotEmpty) {
  KalshiParser p;
  // A present-but-corrupt side must not read as "legal empty book": that
  // would wipe the resident book while reporting Ok.
  const auto r = p.parse(R"({
    "type": "orderbook_snapshot", "sid": 1, "seq": 1,
    "msg": { "market_ticker": "X", "yes": "oops", "no": [] }
  })", 0);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
}

TEST(KalshiParser, OutOfRangePricesAreMalformed) {
  KalshiParser p;
  // 100 - 2147483648 would overflow the folded int; 0 and 100 are outside
  // the contract range. All must be counted, never folded into a level.
  EXPECT_EQ(p.parse(R"({"type":"orderbook_snapshot","sid":1,"seq":1,
    "msg":{"market_ticker":"X","yes":[],"no":[[2147483648,80]]}})", 0).status,
            ParseStatus::Malformed);
  EXPECT_EQ(p.parse(R"({"type":"orderbook_delta","sid":1,"seq":2,
    "msg":{"market_ticker":"X","price":100,"delta":1,"side":"no"}})", 0).status,
            ParseStatus::Malformed);
  EXPECT_EQ(p.parse(R"({"type":"orderbook_delta","sid":1,"seq":3,
    "msg":{"market_ticker":"X","price":0,"delta":1,"side":"yes"}})", 0).status,
            ParseStatus::Malformed);
}

TEST(KalshiParser, MalformedInputIsFlaggedNotDropped) {
  KalshiParser p;
  EXPECT_EQ(p.parse("not json at all", 0).status, ParseStatus::Malformed);
  EXPECT_EQ(p.parse(R"({"type": "orderbook_delta", "msg": {}})", 0).status,
            ParseStatus::Malformed);
  // Snapshot with a corrupt level: nothing partial leaks out.
  const auto r = p.parse(R"({
    "type": "orderbook_snapshot", "seq": 1,
    "msg": { "market_ticker": "X", "yes": [["bad", 1]] }
  })", 0);
  EXPECT_EQ(r.status, ParseStatus::Malformed);
  EXPECT_TRUE(r.deltas.empty());
}
