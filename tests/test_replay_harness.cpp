#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "bench/replay_harness.h"
#include "normalize/contract_registry.h"

using basis::api::InProcessSession;
using basis::api::Update;
using basis::bench::ReplayHarness;
using basis::normalize::TomlContractRegistry;

namespace {

TomlContractRegistry make_registry() {
  auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "fed-cut-2026-09"
kalshi = "FED-26SEP-CUT"
polymarket_token = "7132107"
)");
  return *reg;
}

// A tiny recorded session in real wire formats: Kalshi snapshot + delta,
// Polymarket book + price_change, one unmapped market, one junk line.
std::string write_fixture() {
  const auto path = testing::TempDir() + "harness.feedlog";
  std::ofstream out(path);
  out << "1000\tkalshi\t"
      << R"({"type":"orderbook_snapshot","sid":1,"seq":5,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","yes":[[47,100]],"no":[[51,60]]}})"
      << "\n";
  out << "2000\tpolymarket\t"
      << R"({"event_type":"book","asset_id":"7132107","market":"0xabc",)"
      << R"("bids":[{"price":"0.44","size":"90"}],)"
      << R"("asks":[{"price":"0.46","size":"70"}]})"
      << "\n";
  // Kalshi YES bid improves 47 -> book mid moves; still contiguous seq.
  out << "3000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":6,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":48,"delta":25,"side":"yes"}})"
      << "\n";
  // A market no registry entry maps: counted, never guessed.
  out << "4000\tkalshi\t"
      << R"({"type":"orderbook_snapshot","sid":2,"seq":1,"msg":{)"
      << R"("market_ticker":"SOMETHING-ELSE","yes":[[10,5]],"no":[]}})"
      << "\n";
  // A subscription ack and one corrupt line.
  out << "5000\tkalshi\t" << R"({"id":2,"type":"subscribed"})" << "\n";
  out << "corrupt line with no tabs\n";
  return path;
}

}  // namespace

TEST(ReplayHarness, RunsThePipelineEndToEnd) {
  const auto reg = make_registry();
  InProcessSession session;
  std::vector<double> basis_updates;
  session.subscribe("fed-cut-2026-09", "basis",
                    [&](const Update& u) { basis_updates.push_back(u.value); });

  ReplayHarness harness(reg, &session);
  const auto stats = harness.run(write_fixture());
  ASSERT_TRUE(stats.has_value());

  // Every line is accounted for.
  EXPECT_EQ(stats->records, 5u);
  EXPECT_EQ(stats->kalshi_messages, 4u);
  EXPECT_EQ(stats->polymarket_messages, 1u);
  EXPECT_EQ(stats->ignored, 1u);          // the subscription ack
  EXPECT_EQ(stats->malformed, 0u);
  EXPECT_EQ(stats->malformed_lines, 1u);  // the corrupt framing line
  EXPECT_EQ(stats->gaps, 0u);
  // The unmapped snapshot expands to clear + one level.
  EXPECT_EQ(stats->unmapped_deltas, 2u);

  // Kalshi book: bid 47/ask 49 -> mid 48. Poly book: 44/46 -> mid 45.
  // After the delta the Kalshi bid improves to 48 -> mid 48.5.
  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];
  EXPECT_EQ(event.event_id, "fed-cut-2026-09");
  EXPECT_GE(event.basis_samples, 2u);
  EXPECT_DOUBLE_EQ(event.basis_last, 48.5 - 45.0);

  // Polymarket stays 44/46 the whole run, so its mean bid-ask spread is
  // exactly 2 cents; Kalshi's YES bid improves 47 -> 48 against a 49 ask, so
  // its spread tightens from 2 to 1, leaving a mean in that range.
  EXPECT_DOUBLE_EQ(event.poly_spread_mean, 2.0);
  EXPECT_GE(event.kalshi_spread_mean, 1.0);
  EXPECT_LE(event.kalshi_spread_mean, 2.0);

  // The api layer saw the same numbers the analytics did.
  ASSERT_FALSE(basis_updates.empty());
  EXPECT_DOUBLE_EQ(basis_updates.back(), 3.5);

  // Latency was measured for every record.
  EXPECT_EQ(stats->latency.count, 5u);
  EXPECT_GT(stats->latency.max_ns, 0);
}

TEST(ReplayHarness, MissingFileReportsError) {
  const auto reg = make_registry();
  ReplayHarness harness(reg);
  std::string error;
  EXPECT_FALSE(harness.run("/nonexistent/nope.feedlog", &error).has_value());
  EXPECT_NE(error.find("cannot open"), std::string::npos);
}

TEST(ReplayHarness, BreakdownIsOffByDefaultAndOnWhenAsked) {
  const auto reg = make_registry();

  ReplayHarness plain(reg);
  const auto without = plain.run(write_fixture());
  ASSERT_TRUE(without.has_value());
  EXPECT_EQ(without->parse_ns_total, 0);       // off: nothing measured
  EXPECT_EQ(without->downstream_ns_total, 0);

  ReplayHarness profiled(reg);
  profiled.set_breakdown(true);
  const auto with = profiled.run(write_fixture());
  ASSERT_TRUE(with.has_value());
  // On: both stages accumulate, and their sum is bounded by the whole
  // measured pipeline time (the two brackets nest inside it).
  EXPECT_GT(with->parse_ns_total, 0);
  EXPECT_GT(with->downstream_ns_total, 0);
  EXPECT_LE(with->parse_ns_total + with->downstream_ns_total,
            with->pipeline_ns);
}
