#include <gtest/gtest.h>

#include "model/fees.h"

using basis::model::kalshi_taker_fee_cents;

// Values checked by hand against the published formula
// ceil(0.07 * C * P * (1 - P)), P in dollars.
TEST(KalshiFees, KnownValues) {
  // 1 contract at 50c: 0.07 * 0.25 = $0.0175 -> rounds up to 2 cents.
  EXPECT_EQ(kalshi_taker_fee_cents(1, 50), 2);
  // 100 at 50c: $1.75 exactly, no rounding.
  EXPECT_EQ(kalshi_taker_fee_cents(100, 50), 175);
  // 10 at 99c: 0.07 * 10 * 0.99 * 0.01 = $0.00693 -> 1 cent.
  EXPECT_EQ(kalshi_taker_fee_cents(10, 99), 1);
  // 25 at 47c: 7 * 25 * 47 * 53 = 435925 -> 43.5925c -> 44 cents.
  EXPECT_EQ(kalshi_taker_fee_cents(25, 47), 44);
  // 10 at 48c: 174720 -> 17.472c -> 18 cents.
  EXPECT_EQ(kalshi_taker_fee_cents(10, 48), 18);
}

TEST(KalshiFees, FeeGrowsWithPMaxAtMidpoint) {
  // P(1-P) peaks at 50c; the fee must be symmetric and hump-shaped.
  EXPECT_EQ(kalshi_taker_fee_cents(100, 30), kalshi_taker_fee_cents(100, 70));
  EXPECT_GT(kalshi_taker_fee_cents(100, 50), kalshi_taker_fee_cents(100, 10));
}

TEST(KalshiFees, DegenerateInputsAreFree) {
  EXPECT_EQ(kalshi_taker_fee_cents(0, 50), 0);
  EXPECT_EQ(kalshi_taker_fee_cents(-5, 50), 0);
  EXPECT_EQ(kalshi_taker_fee_cents(10, 0), 0);
  EXPECT_EQ(kalshi_taker_fee_cents(10, 100), 0);
}
