#include <gtest/gtest.h>

#include "normalize/normalizer.h"

using basis::model::BookDelta;
using basis::model::Side;
using basis::model::Venue;
using basis::normalize::Normalizer;
using basis::normalize::TomlContractRegistry;

namespace {

TomlContractRegistry make_registry() {
  auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "fed-cut-2026-09"
kalshi = "FED-26SEP-CUT"
polymarket_token = "7132107"
polymarket_no_token = "9004411"
)");
  return *reg;
}

BookDelta delta(Venue venue, const char* market, Side side, int price,
                std::int64_t size) {
  return BookDelta{.venue = venue,
                   .market = market,
                   .side = side,
                   .price_cents = price,
                   .size = size};
}

}  // namespace

TEST(Normalizer, RoutesBothVenuesIntoOneEventBook) {
  const auto reg = make_registry();
  Normalizer n(reg);

  EXPECT_TRUE(n.on_delta(delta(Venue::Kalshi, "FED-26SEP-CUT", Side::Bid, 47, 100)));
  EXPECT_TRUE(n.on_delta(delta(Venue::Kalshi, "FED-26SEP-CUT", Side::Ask, 49, 100)));
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "7132107", Side::Bid, 44, 100)));
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "7132107", Side::Ask, 46, 100)));

  const auto* book = n.book("fed-cut-2026-09");
  ASSERT_NE(book, nullptr);
  ASSERT_TRUE(book->basis().has_value());
  EXPECT_DOUBLE_EQ(*book->basis(), 48.0 - 45.0);
}

TEST(Normalizer, NoTokenDeltasFoldIntoTheYesFrame) {
  const auto reg = make_registry();
  Normalizer n(reg);

  // A NO bid at 53 is a YES ask at 47; a NO ask at 56 is a YES bid at 44.
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "9004411", Side::Bid, 53, 80)));
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "9004411", Side::Ask, 56, 60)));

  const auto* book = n.book("fed-cut-2026-09");
  ASSERT_NE(book, nullptr);
  const auto& poly = book->book(Venue::Polymarket);
  ASSERT_TRUE(poly.best_ask().has_value());
  EXPECT_EQ(*poly.best_ask(), 47);
  ASSERT_TRUE(poly.best_bid().has_value());
  EXPECT_EQ(*poly.best_bid(), 44);
}

TEST(Normalizer, MirroredSetsFromBothTokensAgreeOnOneLevel) {
  const auto reg = make_registry();
  Normalizer n(reg);

  // The venue matches a NO buy as a YES sell, so both wire views describe
  // the same level. Set semantics make applying both idempotent.
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "7132107", Side::Ask, 47, 80)));
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "9004411", Side::Bid, 53, 80)));

  const auto& poly = n.book("fed-cut-2026-09")->book(Venue::Polymarket);
  EXPECT_EQ(*poly.best_ask(), 47);
  // Removal through the mirror works the same way: NO side drops the bid.
  EXPECT_TRUE(n.on_delta(delta(Venue::Polymarket, "9004411", Side::Bid, 53, 0)));
  EXPECT_FALSE(poly.best_ask().has_value());
}

TEST(Normalizer, ObserverSeesTheFoldedDelta) {
  const auto reg = make_registry();
  Normalizer n(reg);

  BookDelta seen;
  n.set_observer([&](const std::string&, const auto&, const BookDelta& d) {
    seen = d;
  });
  n.on_delta(delta(Venue::Polymarket, "9004411", Side::Bid, 53, 80));
  EXPECT_EQ(seen.side, Side::Ask);
  EXPECT_EQ(seen.price_cents, 47);
  EXPECT_EQ(seen.size, 80);
}

TEST(Normalizer, UnmappedMarketsAreCountedNotGuessed) {
  const auto reg = make_registry();
  Normalizer n(reg);
  EXPECT_FALSE(n.on_delta(delta(Venue::Kalshi, "SOME-OTHER", Side::Bid, 50, 1)));
  EXPECT_EQ(n.unmapped_deltas(), 1u);
  EXPECT_EQ(n.book("SOME-OTHER"), nullptr);
}

TEST(Normalizer, ObserverFiresAfterApply) {
  const auto reg = make_registry();
  Normalizer n(reg);

  std::string seen_event;
  int calls = 0;
  n.set_observer([&](const std::string& event_id, const auto& book,
                     const BookDelta& d) {
    seen_event = event_id;
    ++calls;
    // The delta is already applied when the observer fires.
    if (calls == 1) {
      EXPECT_EQ(*book.book(Venue::Kalshi).best_bid(), 47);
      EXPECT_EQ(d.price_cents, 47);
    }
  });

  n.on_delta(delta(Venue::Kalshi, "FED-26SEP-CUT", Side::Bid, 47, 100));
  n.on_delta(delta(Venue::Kalshi, "SOMETHING-UNMAPPED", Side::Bid, 1, 1));
  EXPECT_EQ(seen_event, "fed-cut-2026-09");
  EXPECT_EQ(calls, 1);  // unmapped deltas never reach the observer
}
