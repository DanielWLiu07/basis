#pragma once

#include <cstdint>

namespace basis::model {

// Kalshi's general fee schedule: taker fees are 0.07 * C * P * (1 - P)
// dollars, rounded up to the next cent (C contracts executed at price P in
// dollars). In integer cents with p in [0, 100]:
//
//   fee_cents = ceil(7 * C * p * (100 - p) / 10000)
//
// Assumptions, stated because they shape every net-edge number downstream:
// maker fees are zero and ignored; the handful of markets on Kalshi's
// reduced schedule are treated as general; Polymarket currently charges no
// taker fee (gas ignored). A cross-venue arb therefore pays fees on the
// Kalshi leg only, at the Kalshi execution price.
std::int64_t kalshi_taker_fee_cents(std::int64_t contracts, int price_cents);

}  // namespace basis::model
