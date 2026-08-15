#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "model/types.h"

namespace basis::normalize {

// Maps a venue's own symbol for a crypto pair onto one canonical name, so
// the same asset quoted on two venues compares as one instrument.
//
// The venues do not agree on spelling. Binance writes "btcusdt": lower
// case, no separator, quote glued to the base. Coinbase writes "BTC-USD":
// upper case, hyphenated. A cross-venue measurement has to pair them, and
// pairing by string equality silently pairs nothing, which is worse than
// failing: two venues that never match produce an empty result that looks
// like a quiet market rather than a bug.
//
// The canonical form is BASE/USD. That collapses USDT and USDC onto USD,
// which is an assumption and not an identity: a dollar stablecoin trades
// near a dollar but is a different instrument with its own credit and
// redemption risk. The whole cross-venue comparison already rests on this
// (Binance quotes BTCUSDT, Coinbase quotes BTC-USD, and the lead-lag
// result treats them as the same asset), so the assumption is made once
// here, by name, instead of being spread implicitly across the analysis.
//
// Returns nullopt for a symbol whose quote is not dollar-like, rather than
// guessing. A BTC/EUR book paired against a BTC/USD one would produce a
// perfectly plausible lead that is mostly the EUR/USD rate.
std::optional<std::string> canonical_instrument(model::Venue venue,
                                                std::string_view market);

}  // namespace basis::normalize
