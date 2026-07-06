// Fuzz surface: one raw Kalshi WebSocket message of attacker-controlled
// bytes. The parser's contract is total: any input is Ok, Ignored, or
// Malformed, never a crash, overflow, or fabricated book level. A fresh
// parser per input keeps every finding reproducible from its bytes alone.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "feed/kalshi_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  basis::feed::KalshiParser parser;
  const std::string_view raw(reinterpret_cast<const char*>(data), size);
  const auto first = parser.parse(raw, 0);
  (void)first;
  // A second parse of the same bytes exercises the stateful paths: the
  // sequence ledger and snapshot set now have entries, which is how the
  // gap and delta-before-snapshot branches get coverage.
  const auto second = parser.parse(raw, 0);
  (void)second;
  return 0;
}
