#include <gtest/gtest.h>

#include "normalize/contract_registry.h"

using basis::model::Venue;
using basis::normalize::TomlContractRegistry;

TEST(TomlContractRegistry, MapsBothVenuesToTheEventId) {
  const auto reg = TomlContractRegistry::parse(R"(
# matched events
[[event]]
id = "fed-cut-2026-09"
description = "Fed cuts rates in September 2026"
kalshi = "FED-26SEP-CUT"
polymarket_token = "7132107"
polymarket_market = "0xabc"

[[event]]
id = "us-pres-2028-dem"
kalshi = "PRES-2028-DEM"
polymarket_token = "9944001"
)");
  ASSERT_TRUE(reg.has_value());
  EXPECT_EQ(reg->event_ids().size(), 2u);
  EXPECT_EQ(*reg->event_id(Venue::Kalshi, "FED-26SEP-CUT"), "fed-cut-2026-09");
  EXPECT_EQ(*reg->event_id(Venue::Polymarket, "7132107"), "fed-cut-2026-09");
  EXPECT_EQ(*reg->event_id(Venue::Kalshi, "PRES-2028-DEM"), "us-pres-2028-dem");
  EXPECT_FALSE(reg->event_id(Venue::Kalshi, "UNKNOWN").has_value());
  // Venue matters: a Kalshi ticker is not a Polymarket token.
  EXPECT_FALSE(reg->event_id(Venue::Polymarket, "FED-26SEP-CUT").has_value());

  // Subscribe keys for the live feeds, in file order.
  ASSERT_EQ(reg->kalshi_tickers().size(), 2u);
  EXPECT_EQ(reg->kalshi_tickers()[0], "FED-26SEP-CUT");
  ASSERT_EQ(reg->polymarket_tokens().size(), 2u);
  EXPECT_EQ(reg->polymarket_tokens()[1], "9944001");
}

TEST(TomlContractRegistry, EventWithoutIdFailsWithLineNumber) {
  std::string error;
  const auto reg = TomlContractRegistry::parse(R"(
[[event]]
kalshi = "FED-26SEP-CUT"
)", &error);
  EXPECT_FALSE(reg.has_value());
  EXPECT_NE(error.find("line 2"), std::string::npos);
  EXPECT_NE(error.find("no id"), std::string::npos);
}

TEST(TomlContractRegistry, DuplicateMappingFails) {
  std::string error;
  const auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "a"
kalshi = "SAME-TICKER"

[[event]]
id = "b"
kalshi = "SAME-TICKER"
)", &error);
  EXPECT_FALSE(reg.has_value());
  EXPECT_NE(error.find("mapped twice"), std::string::npos);
}

TEST(TomlContractRegistry, MalformedLinesFailLoudly) {
  std::string error;
  EXPECT_FALSE(TomlContractRegistry::parse("id = \"orphan\"\n", &error)
                   .has_value());
  EXPECT_NE(error.find("outside"), std::string::npos);

  EXPECT_FALSE(TomlContractRegistry::parse(
                   "[[event]]\nid = unquoted\n", &error)
                   .has_value());
  EXPECT_NE(error.find("quoted"), std::string::npos);
}

TEST(TomlContractRegistry, LoadsTheCheckedInConfig) {
  std::string error;
  const auto reg = TomlContractRegistry::load(BASIS_SOURCE_DIR
                                              "/configs/contracts.toml",
                                              &error);
  ASSERT_TRUE(reg.has_value()) << error;
  EXPECT_GE(reg->event_ids().size(), 1u);
}
