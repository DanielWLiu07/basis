#include "model/fees.h"

#include <algorithm>
#include <limits>

namespace basis::model {

std::int64_t kalshi_taker_fee_cents(std::int64_t contracts, int price_cents) {
  if (contracts <= 0 || price_cents <= 0 || price_cents >= 100) return 0;
  // fee = ceil(kFeeNumerator * C * p * (100 - p) / kFeeDenominator), the
  // integer-cent form of the schedule in the header.
  constexpr std::int64_t kFeeNumerator = 7;
  constexpr std::int64_t kFeeDenominator = 10000;
  // p * (100 - p) is maximized at p = 50.
  constexpr std::int64_t kMaxPriceFactor = 50 * 50;
  // Book sizes saturate at int64 max upstream (OrderBook::apply), so this
  // multiply must not trust its input range: the product overflows int64
  // above ~5.3e14 contracts, and signed overflow is undefined behavior.
  // Cap where the arithmetic stays exact at the worst-case price factor;
  // the fee is monotone in contracts, so the capped result is a lower
  // bound on the true, astronomical fee.
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kContractsCap =
      (kMax - (kFeeDenominator - 1)) / (kFeeNumerator * kMaxPriceFactor);
  contracts = std::min(contracts, kContractsCap);
  const std::int64_t numer =
      kFeeNumerator * contracts * price_cents * (100 - price_cents);
  return (numer + kFeeDenominator - 1) / kFeeDenominator;
}

}  // namespace basis::model
