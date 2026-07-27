#include "model/fees.h"

namespace basis::model {

std::int64_t kalshi_taker_fee_cents(std::int64_t contracts, int price_cents) {
  if (contracts <= 0 || price_cents <= 0 || price_cents >= 100) return 0;
  const std::int64_t numer =
      7 * contracts * price_cents * (100 - price_cents);
  return (numer + 9999) / 10000;  // ceil
}

}  // namespace basis::model
