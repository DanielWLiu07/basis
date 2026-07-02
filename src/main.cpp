#include <cstdio>

#include "core/logger.h"
#include "core/version.h"
#include "model/order_book.h"

// Interface headers for the per-phase layers are included here so the design
// stubs are kept honest: they must parse and stay consistent with model/ even
// before their sources exist.
#include "analytics/lead_lag.h"
#include "api/subscription.h"
#include "feed/feed_adapter.h"
#include "normalize/normalizer.h"

int main() {
  using namespace basis;

  log::info("basis - cross-venue prediction-market data engine");
  std::printf("  version  %s\n", kVersion);
  std::printf("  pipeline feed -> normalize -> unified book -> analytics -> api\n");
  std::printf("  venues   %s, %s\n",
              model::to_string(model::Venue::Kalshi),
              model::to_string(model::Venue::Polymarket));

  // Phase-0 self-check: the canonical OrderBook applies deltas and computes a
  // midpoint. This is the seed the rest of the engine is built around.
  model::OrderBook book;
  book.apply({model::Venue::Kalshi, "PRES-2028-DEM", model::Side::Bid, 47, 1200, 1, 0});
  book.apply({model::Venue::Kalshi, "PRES-2028-DEM", model::Side::Ask, 49, 800, 2, 0});
  if (const auto m = book.mid()) {
    std::printf("  self-check mid %.1fc (expect 48.0c)\n", *m);
  }

  log::info("scaffold OK - see PLAN.md for the build phases");
  return 0;
}
