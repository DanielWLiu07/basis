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
