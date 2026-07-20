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

  // Kalshi's best bid (47, then 48) sits above Polymarket's best ask (46) for
  // the whole run, so every two-sided update is a crossable dislocation: one
  // unbroken episode spanning the two-sided updates at ts 2000 and 3000.
  EXPECT_GT(event.two_sided_updates, 0u);
  EXPECT_EQ(event.crossable_updates, event.two_sided_updates);
  EXPECT_EQ(event.crossable_episodes, 1u);
  EXPECT_EQ(event.crossable_longest_ns, 1000);

  // The api layer saw the same numbers the analytics did.
  ASSERT_FALSE(basis_updates.empty());
  EXPECT_DOUBLE_EQ(basis_updates.back(), 3.5);

  // Latency was measured for every record.
  EXPECT_EQ(stats->latency.count, 5u);
  EXPECT_GT(stats->latency.max_ns, 0);
}

// Books that cross, uncross, and cross again must count as two distinct
// episodes, with the longest span measured within a single run only.
std::string write_episode_fixture() {
  const auto path = testing::TempDir() + "episodes.feedlog";
  std::ofstream out(path);
  // Kalshi bid 45 / ask 47; Polymarket 44 / 46: two-sided, not crossed.
  out << "1000\tkalshi\t"
      << R"({"type":"orderbook_snapshot","sid":1,"seq":5,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","yes":[[45,100]],"no":[[53,60]]}})"
      << "\n";
  out << "2000\tpolymarket\t"
      << R"({"event_type":"book","asset_id":"7132107","market":"0xabc",)"
      << R"("bids":[{"price":"0.44","size":"90"}],)"
      << R"("asks":[{"price":"0.46","size":"70"}]})"
      << "\n";
  // Bid lifts to 47 > poly ask 46: crossed (episode 1 opens).
  out << "3000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":6,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":47,"delta":25,"side":"yes"}})"
      << "\n";
  // That bid pulls: back to 45, uncrossed (episode 1 closes at span 0).
  out << "4000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":7,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":47,"delta":-25,"side":"yes"}})"
      << "\n";
  // A 48 bid appears: crossed again (episode 2 opens at ts 5000).
  out << "5000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":8,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":48,"delta":10,"side":"yes"}})"
      << "\n";
  // A deep bid ticks while the top stays crossed: episode 2 extends to 1000ns.
  out << "6000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":9,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":30,"delta":5,"side":"yes"}})"
      << "\n";
  return path;
}

TEST(ReplayHarness, CountsDistinctCrossedEpisodesAndTheirSpan) {
  const auto reg = make_registry();
  ReplayHarness harness(reg);
  const auto stats = harness.run(write_episode_fixture());
  ASSERT_TRUE(stats.has_value());
  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];

  // Two-sided from the Polymarket book onward: ts 2000 through 6000.
  EXPECT_EQ(event.two_sided_updates, 5u);
  // Crossed at ts 3000, then 5000 and 6000.
  EXPECT_EQ(event.crossable_updates, 3u);
  // The uncross at ts 4000 splits those into two runs; the second spans
  // ts 5000 to 6000, and the first was only ever seen at one update.
  EXPECT_EQ(event.crossable_episodes, 2u);
  EXPECT_EQ(event.crossable_longest_ns, 1000);
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
