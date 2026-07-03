// Compiled only when BASIS_ENABLE_BDE is on (see tests/CMakeLists.txt).
// Exercises the bdlma-backed resources through the same seams the replay
// harness uses: a parser fed a per-message arena and an order book whose
// nodes come from a multipool.

#include <gtest/gtest.h>

#include <memory_resource>

#include "alloc/bde_arena.h"
#include "feed/polymarket_parser.h"
#include "model/order_book.h"

namespace {

using basis::alloc::BdeMultipool;
using basis::alloc::BdeSequentialArena;

constexpr std::string_view kBook = R"({
  "event_type": "book",
  "asset_id": "10711150627379254398604222438842748670283066561128118311100672676687834647536",
  "bids": [{"price": "0.45", "size": "100"}, {"price": "0.44", "size": "50"}],
  "asks": [{"price": "0.47", "size": "80"}]
})";

TEST(BdeSequentialArena, BacksParseResultsAcrossReleaseCycles) {
  BdeSequentialArena arena;
  basis::feed::PolymarketParser parser;

  for (int cycle = 0; cycle < 1000; ++cycle) {
    const auto r = parser.parse(kBook, cycle, arena.resource());
    ASSERT_EQ(r.deltas.size(), 4u);  // clear + 3 levels
    EXPECT_EQ(r.deltas[1].price_cents, 45);
    ASSERT_TRUE(r.deltas.get_allocator().resource()->is_equal(
        *arena.resource()));
    arena.release();  // r must not be touched past this point
  }
}

TEST(BdeMultipool, BacksOrderBookNodeChurn) {
  BdeMultipool pool;
  basis::model::OrderBook book(pool.resource());

  for (int cycle = 0; cycle < 1000; ++cycle) {
    for (int price = 1; price < 100; ++price) {
      book.apply({.side = basis::model::Side::Bid,
                  .price_cents = price,
                  .size = cycle + price});
    }
    for (int price = 1; price < 100; price += 2) {
      book.apply({.side = basis::model::Side::Bid,
                  .price_cents = price,
                  .size = 0});  // Set to zero removes the level
    }
  }
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(*book.best_bid(), 98);
}

}  // namespace
