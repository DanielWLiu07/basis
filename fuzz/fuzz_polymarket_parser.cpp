// Fuzz surface: one raw Polymarket WebSocket message of attacker-controlled
// bytes, covering the hand-rolled decimal parsing (the classic overflow
// spot), the book/price_change dispatch, and the array-of-events framing.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "feed/polymarket_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  basis::feed::PolymarketParser parser;
  const std::string_view raw(reinterpret_cast<const char*>(data), size);
  const auto result = parser.parse(raw, 0);
  (void)result;
  return 0;
}
