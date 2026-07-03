#include <gtest/gtest.h>

#include <memory_resource>
#include <vector>

#include "core/counting_resource.h"
#include "feed/polymarket_parser.h"

namespace {

using basis::core::CountingResource;

TEST(CountingResource, CountsTrafficAndForwardsUpstream) {
  CountingResource counting;
  std::pmr::vector<int> v(&counting);
  v.reserve(100);
  EXPECT_EQ(counting.allocations(), 1u);
  EXPECT_EQ(counting.bytes(), 100 * sizeof(int));
  v.shrink_to_fit();
  v = {};
  EXPECT_EQ(counting.deallocations(), counting.allocations());
}

// Pins the allocation budget of the hot path: one message through the real
// parser costs the deltas vector and nothing else. If a change reintroduces
// a per-delta string copy or a hidden temporary, this fails loudly instead
// of quietly showing up in a benchmark months later.
TEST(CountingResource, ParseOfALargeBookStaysWithinBudget) {
  constexpr std::string_view kBook = R"({
    "event_type": "book",
    "asset_id": "10711150627379254398604222438842748670283066561128118311100672676687834647536",
    "bids": [{"price": "0.45", "size": "100"}, {"price": "0.44", "size": "50"},
             {"price": "0.43", "size": "25"}, {"price": "0.42", "size": "10"}],
    "asks": [{"price": "0.47", "size": "80"}, {"price": "0.48", "size": "40"}]
  })";

  basis::feed::PolymarketParser parser;
  CountingResource counting;
  const auto r = parser.parse(kBook, 0, &counting);
  ASSERT_EQ(r.deltas.size(), 7u);  // clear + 6 levels

  // Geometric growth of the deltas vector: 1 -> 2 -> 4 -> 8 elements.
  EXPECT_LE(counting.allocations(), 4u);
}

}  // namespace
