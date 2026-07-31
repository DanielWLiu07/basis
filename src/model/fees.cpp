#include "model/fees.h"

#include <algorithm>
#include <limits>

namespace basis::model {

std::int64_t kalshi_taker_fee_cents(std::int64_t contracts, int price_cents) {
  if (contracts <= 0 || price_cents <= 0 || price_cents >= 100) return 0;
  // Book sizes saturate at int64 max upstream (OrderBook::apply), so this
  // multiply must not trust its input range: 7 * C * p * (100 - p)
  // overflows int64 above ~5.3e14 contracts, and signed overflow is
  // undefined behavior. Cap where the arithmetic stays exact at the
  // worst-case price factor (p = 50); the fee is monotone in contracts,
  // so the capped result is a lower bound on the true, astronomical fee.
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kContractsCap = (kMax - 9999) / (7 * 2500);
  contracts = std::min(contracts, kContractsCap);
  const std::int64_t numer =
      7 * contracts * price_cents * (100 - price_cents);
  return (numer + 9999) / 10000;  // ceil
}

}  // namespace basis::model
