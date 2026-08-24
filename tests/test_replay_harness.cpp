#include <gtest/gtest.h>

#include <chrono>
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
  // Depth: Kalshi bid 47 over Polymarket ask 46 is 1c, then the bid lifts
  // to 48 for 2c; mean 1.5c, max 2c.
  EXPECT_DOUBLE_EQ(event.crossable_depth_mean, 1.5);
  EXPECT_DOUBLE_EQ(event.crossable_depth_max, 2.0);
  // Edge at the touch: 1c * min(100 bid, 70 ask) = $0.70, then the fresh
  // 48 bid has only 25 resting: 2c * min(25, 70) = $0.50.
  EXPECT_DOUBLE_EQ(event.crossable_edge_max_dollars, 0.70);
  EXPECT_DOUBLE_EQ(event.crossable_edge_mean_dollars, 0.60);
  // All updates land within microseconds of each other: nothing is stale.
  EXPECT_EQ(event.stale_basis_samples, 0u);

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
  // Depths over the three crossed updates: 47 over 46 (1c), then 48 over
  // 46 twice (2c each).
  EXPECT_DOUBLE_EQ(event.crossable_depth_mean, 5.0 / 3.0);
  EXPECT_DOUBLE_EQ(event.crossable_depth_max, 2.0);
  // Touch edge: 1c * min(25, 70) = $0.25, then 2c * min(10, 70) = $0.20
  // twice (the deep 30 bid never changes the touch).
  EXPECT_DOUBLE_EQ(event.crossable_edge_max_dollars, 0.25);
  EXPECT_DOUBLE_EQ(event.crossable_edge_mean_dollars, 0.65 / 3.0);

  // Net of Kalshi taker fees, the picture inverts for the small cross:
  // ts 3000 grosses 1c * 25 = $0.25 but the Kalshi leg at 47c costs
  // ceil(7*25*47*53/10000) = 44c -> net -$0.19; ts 5000/6000 gross
  // 2c * 10 = $0.20 against an 18c fee at 48c -> net +$0.02 each. Only
  // the deeper cross survives fees.
  EXPECT_EQ(event.crossable_profitable_updates, 2u);
  EXPECT_NEAR(event.crossable_net_edge_max_dollars, 0.02, 1e-9);
  EXPECT_NEAR(event.crossable_net_edge_mean_dollars, -0.05, 1e-9);

  // The optimal fee-aware sweep never goes negative: at ts 3000 it
  // declines the losing fill entirely (0, not -$0.19), at ts 5000/6000 it
  // takes the profitable touch fill for +$0.02. Two of three crossed
  // updates had anything worth taking.
  EXPECT_EQ(event.crossable_sweepable_updates, 2u);
  EXPECT_NEAR(event.crossable_net_sweep_max_dollars, 0.02, 1e-9);
  EXPECT_NEAR(event.crossable_net_sweep_mean_dollars, 0.04 / 3.0, 1e-9);

  // Both episodes here live on a nanosecond scale, so every reaction rung
  // beyond "at the open" expires: alive counts drop to zero and the
  // expected surviving edge with them. At the open, episode one's only
  // fill is fee-eaten (declined, $0) while episode two opens at +$0.02.
  EXPECT_EQ(event.episodes_alive_after[0], 2u);
  EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[0], 0.01, 1e-9);
  for (int t = 1; t < basis::bench::ReplayStats::kReactionTaus; ++t) {
    EXPECT_EQ(event.episodes_alive_after[t], 0u) << "tau index " << t;
    EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[t], 0.0, 1e-9)
        << "tau index " << t;
  }

  // The per-episode records carry the distribution behind those
  // aggregates, in time order and consistent with them.
  ASSERT_EQ(event.episodes.size(), event.crossable_episodes);
  const auto& first = event.episodes[0];
  EXPECT_EQ(first.start_ns, 3000);
  EXPECT_EQ(first.end_ns, 3000);  // single-update run spans 0
  EXPECT_EQ(first.updates, 1u);
  EXPECT_DOUBLE_EQ(first.depth_max_cents, 1.0);
  EXPECT_DOUBLE_EQ(first.edge_max_dollars, 0.25);
  const auto& second = event.episodes[1];
  EXPECT_EQ(second.start_ns, 5000);
  EXPECT_EQ(second.end_ns, 6000);
  EXPECT_EQ(second.updates, 2u);
  EXPECT_DOUBLE_EQ(second.depth_max_cents, 2.0);
  EXPECT_DOUBLE_EQ(second.edge_max_dollars, 0.20);
  EXPECT_EQ(first.updates + second.updates, event.crossable_updates);
  EXPECT_EQ(second.end_ns - second.start_ns, event.crossable_longest_ns);
}

// A venue going quiet must show up as staleness, not as a live basis: the
// Kalshi delta 7 seconds after Polymarket's last update prices the basis
// against a 7s-old book.
std::string write_stale_fixture() {
  const auto path = testing::TempDir() + "stale.feedlog";
  std::ofstream out(path);
  out << "1000\tkalshi\t"
      << R"({"type":"orderbook_snapshot","sid":1,"seq":5,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","yes":[[45,100]],"no":[[53,60]]}})"
      << "\n";
  out << "2000\tpolymarket\t"
      << R"({"event_type":"book","asset_id":"7132107","market":"0xabc",)"
      << R"("bids":[{"price":"0.44","size":"90"}],)"
      << R"("asks":[{"price":"0.46","size":"70"}]})"
      << "\n";
  out << "7000002000\tkalshi\t"
      << R"({"type":"orderbook_delta","sid":1,"seq":6,"msg":{)"
      << R"("market_ticker":"FED-26SEP-CUT","price":44,"delta":5,"side":"yes"}})"
      << "\n";
  return path;
}

TEST(ReplayHarness, FlagsBasisSamplesPricedOnStaleQuotes) {
  const auto reg = make_registry();
  ReplayHarness harness(reg);
  const auto stats = harness.run(write_stale_fixture());
  ASSERT_TRUE(stats.has_value());
  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];

  // The Polymarket book at ts 2000 prices against a 1000ns-old Kalshi
  // quote (fresh); the Kalshi delta at ts ~7s prices against Polymarket's
  // ts-2000 book, 7 seconds stale.
  EXPECT_GE(event.basis_samples, 2u);
  EXPECT_EQ(event.stale_basis_samples, 1u);
  EXPECT_NEAR(event.stalest_quote_seconds, 7.0, 1e-6);
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

TEST(ReplayHarness, UnpacedReplayReportsNoResponseTime) {
  // The historical path. Flat out, the harness cannot know when a record
  // was due, so it must leave response time empty rather than fill it
  // with something that looks like an answer.
  const auto reg = make_registry();
  ReplayHarness harness(reg);
  const auto stats = harness.run(write_fixture());
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->replay_speed, 0.0);
  EXPECT_EQ(stats->response_latency.count, 0u);
  EXPECT_EQ(stats->records_late, 0u);
  EXPECT_EQ(stats->max_lag_ns, 0);
  EXPECT_GT(stats->latency.count, 0u);  // service time still measured
}

TEST(ReplayHarness, PacedReplayMeasuresEveryRecordsResponseTime) {
  // The fixture's timestamps span 4 us, so even at 1x this finishes
  // instantly; the assertions are on the accounting, not on the clock.
  // Every record gets a response sample, and lateness can never exceed
  // the record count.
  const auto reg = make_registry();
  ReplayHarness harness(reg);
  harness.set_replay_speed(1.0);
  const auto stats = harness.run(write_fixture());
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->replay_speed, 1.0);
  EXPECT_EQ(stats->response_latency.count, stats->latency.count);
  EXPECT_LE(stats->records_late, stats->records);
  EXPECT_GE(stats->max_lag_ns, 0);
}

TEST(ReplayHarness, PacingActuallyWaits) {
  // 200 records two milliseconds apart is a 400 ms schedule at 1x.
  //
  // The assertion is a LOWER bound on elapsed wall time, deliberately. An
  // earlier version of this test asserted that few records ran late,
  // which is a statement about the machine rather than about the code:
  // the ASan job on a shared runner could not meet it and the build went
  // red for a correct pacer. A slow or noisy machine can only make this
  // run take LONGER, so a lower bound cannot be tripped by load - only by
  // a pacer that stopped waiting, which is the regression worth catching.
  const auto path = testing::TempDir() + "paced.feedlog";
  constexpr int kRecords = 200;
  constexpr std::int64_t kSpacingNs = 2'000'000;  // 2 ms
  {
    std::ofstream out(path);
    for (int i = 0; i < kRecords; ++i) {
      out << (kSpacingNs * (i + 1)) << "\tkalshi\t"
          << R"({"type":"orderbook_delta","sid":1,"seq":)" << (i + 6)
          << R"(,"msg":{"market_ticker":"FED-26SEP-CUT","price":48,)"
          << R"("delta":1,"side":"yes"}})" << "\n";
    }
  }
  const auto reg = make_registry();

  ReplayHarness harness(reg);
  harness.set_replay_speed(1.0);
  const auto start = std::chrono::steady_clock::now();
  const auto stats = harness.run(path);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->records, static_cast<std::uint64_t>(kRecords));
  EXPECT_EQ(stats->response_latency.count,
            static_cast<std::uint64_t>(kRecords));

  // 60% of the schedule: comfortably above what an unpaced run of 200
  // tiny deltas takes (microseconds), comfortably below the schedule
  // itself so a little clock jitter cannot fail it.
  const auto schedule = std::chrono::nanoseconds(kSpacingNs * kRecords);
  EXPECT_GT(elapsed, schedule * 6 / 10)
      << "paced replay finished too fast to have waited";

  // And the same file unpaced has to be far quicker, or the bound above
  // proves nothing about pacing.
  ReplayHarness flat_out(reg);
  const auto t0 = std::chrono::steady_clock::now();
  const auto unpaced = flat_out.run(path);
  const auto flat_elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(unpaced.has_value());
  EXPECT_LT(flat_elapsed, elapsed);
}

// summarize() is a pure function of ReplayStats, so it can be pinned with a
// hand-built input rather than a replay fixture.
TEST(ReplaySummary, TotalsRankingAndTruncation) {
  using basis::bench::ReplayStats;
  ReplayStats stats;

  const auto add = [&](const char* id, std::uint64_t samples,
                       std::uint64_t crossable, std::uint64_t episodes,
                       std::int64_t longest_ns, double edge_mean,
                       double edge_max) {
    ReplayStats::EventReport e;
    e.event_id = id;
    e.basis_samples = samples;
    e.crossable_updates = crossable;
    e.crossable_episodes = episodes;
    e.crossable_longest_ns = longest_ns;
    e.crossable_edge_mean_dollars = edge_mean;
    e.crossable_edge_max_dollars = edge_max;
    // Distinct survival values per event so the copy into RankedEvent is
    // pinned (index 2 = the 100 ms rung).
    e.episode_net_sweep_after_mean_dollars[2] = edge_mean / 2.0;
    e.episodes_alive_after[2] = episodes;
    stats.events.push_back(std::move(e));
  };

  add("alpha", 100, 10, 2, 5'000'000, 0.30, 0.50);
  add("bravo", 200, 4, 1, 1'000'000, 0.80, 1.20);
  add("charlie", 50, 0, 0, 0, 0.0, 0.0);  // overlap but never crossed
  add("delta", 0, 0, 0, 0, 0.0, 0.0);     // no overlap at all

  const auto s = basis::bench::summarize(stats);
  EXPECT_EQ(s.events_tracked, 4u);
  EXPECT_EQ(s.events_with_overlap, 3u);
  EXPECT_EQ(s.events_crossable, 2u);
  EXPECT_EQ(s.basis_samples, 350u);
  EXPECT_EQ(s.crossable_updates, 14u);
  EXPECT_EQ(s.crossable_episodes, 3u);

  // Ranked by max edge, not by activity: bravo's $1.20 beats alpha's $0.50
  // even though alpha crossed more often.
  ASSERT_EQ(s.top_by_edge.size(), 2u);
  EXPECT_EQ(s.top_by_edge[0].event_id, "bravo");
  EXPECT_EQ(s.top_by_edge[1].event_id, "alpha");
  EXPECT_DOUBLE_EQ(s.top_by_edge[0].edge_max_dollars, 1.20);
  EXPECT_EQ(s.top_by_edge[0].crossable_updates, 4u);
  EXPECT_EQ(s.top_by_edge[0].crossable_longest_ns, 1'000'000);
  // The executability columns ride along with the ranking untouched.
  EXPECT_DOUBLE_EQ(s.top_by_edge[0].surviving_100ms_mean_dollars, 0.40);
  EXPECT_EQ(s.top_by_edge[0].episodes_alive_100ms, 1u);
  EXPECT_DOUBLE_EQ(s.top_by_edge[1].surviving_100ms_mean_dollars, 0.15);
  EXPECT_EQ(s.top_by_edge[1].episodes_alive_100ms, 2u);

  // top_n caps the list after ranking, so the best event survives the cut.
  const auto top1 = basis::bench::summarize(stats, 1);
  ASSERT_EQ(top1.top_by_edge.size(), 1u);
  EXPECT_EQ(top1.top_by_edge[0].event_id, "bravo");
}

TEST(ReplaySummary, EqualEdgesRankDeterministicallyById) {
  using basis::bench::ReplayStats;
  ReplayStats stats;
  for (const char* id : {"zulu", "mike", "echo"}) {
    ReplayStats::EventReport e;
    e.event_id = id;
    e.basis_samples = 10;
    e.crossable_updates = 1;
    e.crossable_episodes = 1;
    e.crossable_edge_mean_dollars = 0.25;
    e.crossable_edge_max_dollars = 0.25;
    stats.events.push_back(std::move(e));
  }
  const auto s = basis::bench::summarize(stats);
  ASSERT_EQ(s.top_by_edge.size(), 3u);
  EXPECT_EQ(s.top_by_edge[0].event_id, "echo");
  EXPECT_EQ(s.top_by_edge[1].event_id, "mike");
  EXPECT_EQ(s.top_by_edge[2].event_id, "zulu");
}

// Two crossed bid levels against one deep ask: the touch edge sees only the
// best levels, the sweep walks the whole crossed depth.
// Timestamps here are realistic (tens to hundreds of ms) so the
// reaction-latency ladder actually has boundaries to cross. The edge
// standing at tau is the value from the last update at or before that
// instant, not the next one.
TEST(ReplayHarness, ReactionDelaySurvivalUsesTheStandingEdge) {
  const auto reg = make_registry();
  const auto path = testing::TempDir() + "survival.feedlog";
  constexpr std::int64_t kT0 = 1'000'000'000;    // uncrossed two-sided base
  constexpr std::int64_t kOpen = kT0 + 20'000'000;
  {
    std::ofstream out(path);
    out << kT0 << "\tkalshi\t"
        << R"({"type":"orderbook_snapshot","sid":1,"seq":5,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","yes":[[45,100]],"no":[[53,60]]}})"
        << "\n";
    out << (kT0 + 10'000'000) << "\tpolymarket\t"
        << R"({"event_type":"book","asset_id":"7132107","market":"0xabc",)"
        << R"("bids":[{"price":"0.44","size":"90"}],)"
        << R"("asks":[{"price":"0.46","size":"70"}]})"
        << "\n";
    // Open: 48 bid crosses the 46 ask. Optimal sweep nets 20c - 18c fee.
    out << kOpen << "\tkalshi\t"
        << R"({"type":"orderbook_delta","sid":1,"seq":6,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","price":48,"delta":10,"side":"yes"}})"
        << "\n";
    // +60ms: a 49 bid deepens the cross; optimal sweep is now 12c + 2c.
    out << (kOpen + 60'000'000) << "\tkalshi\t"
        << R"({"type":"orderbook_delta","sid":1,"seq":7,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","price":49,"delta":10,"side":"yes"}})"
        << "\n";
    // +120ms and +300ms: deep bids tick without changing the cross, so
    // the standing value carries across the 100ms and 250ms boundaries.
    out << (kOpen + 120'000'000) << "\tkalshi\t"
        << R"({"type":"orderbook_delta","sid":1,"seq":8,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","price":30,"delta":5,"side":"yes"}})"
        << "\n";
    out << (kOpen + 300'000'000) << "\tkalshi\t"
        << R"({"type":"orderbook_delta","sid":1,"seq":9,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","price":31,"delta":5,"side":"yes"}})"
        << "\n";
  }
  ReplayHarness harness(reg);
  const auto stats = harness.run(path);
  ASSERT_TRUE(stats.has_value());
  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];
  ASSERT_EQ(event.crossable_episodes, 1u);
  // Alive at every rung: the episode outlives 250ms.
  for (int t = 0; t < basis::bench::ReplayStats::kReactionTaus; ++t) {
    EXPECT_EQ(event.episodes_alive_after[t], 1u) << "tau index " << t;
  }
  // At the open the sweep nets $0.02; that value stands through the 50ms
  // boundary (the deepening at +60ms comes after it); the deepened $0.14
  // stands at 100ms and 250ms.
  EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[0], 0.02, 1e-9);
  EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[1], 0.02, 1e-9);
  EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[2], 0.14, 1e-9);
  EXPECT_NEAR(event.episode_net_sweep_after_mean_dollars[3], 0.14, 1e-9);
}

TEST(ReplayHarness, SweepEdgeWalksPastTheTouch) {
  const auto reg = make_registry();
  const auto path = testing::TempDir() + "sweep.feedlog";
  {
    std::ofstream out(path);
    // Kalshi: bids 48x10 and 47x20 (no 51 -> ask 49, uncrossed internally).
    out << "1000\tkalshi\t"
        << R"({"type":"orderbook_snapshot","sid":1,"seq":5,"msg":{)"
        << R"("market_ticker":"FED-26SEP-CUT","yes":[[48,10],[47,20]],)"
        << R"("no":[[51,60]]}})"
        << "\n";
    // Polymarket ask 46x70 crosses both kalshi bids.
    out << "2000\tpolymarket\t"
        << R"({"event_type":"book","asset_id":"7132107","market":"0xabc",)"
        << R"("bids":[{"price":"0.44","size":"90"}],)"
        << R"("asks":[{"price":"0.46","size":"70"}]})"
        << "\n";
  }
  ReplayHarness harness(reg);
  const auto stats = harness.run(path);
  ASSERT_TRUE(stats.has_value());
  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];
  EXPECT_EQ(event.crossable_updates, 1u);
  // Touch: 2c * min(10, 70) = $0.20. Sweep: 2c * 10 + 1c * 20 = $0.40.
  EXPECT_DOUBLE_EQ(event.crossable_edge_max_dollars, 0.20);
  EXPECT_DOUBLE_EQ(event.crossable_sweep_max_dollars, 0.40);
  EXPECT_GE(event.crossable_sweep_mean_dollars,
            event.crossable_edge_mean_dollars);
  // The gross sweep promised $0.40; the fee-aware optimal sweep keeps the
  // touch fill (20c - 18c fee = +$0.02) and skips the deeper one
  // (20c - 35c fee), so a taker executes 10 of the 30 crossed contracts.
  EXPECT_EQ(event.crossable_sweepable_updates, 1u);
  EXPECT_NEAR(event.crossable_net_sweep_max_dollars, 0.02, 1e-9);
  EXPECT_LE(event.crossable_net_sweep_max_dollars,
            event.crossable_sweep_max_dollars);
}

// Two mutually exclusive outcomes in one basket, quoted on Polymarket.
// The basket samples only when BOTH books are two-sided; the max
// two-sided count explains a zero-sample basket instead of hiding it.
TEST(ReplayHarness, BasketSumsSampleOnlyWhenFullyQuoted) {
  auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "demo-yes"
basket = "demo"
polymarket_token = "1001"

[[event]]
id = "demo-no"
basket = "demo"
polymarket_token = "1002"
)");
  ASSERT_TRUE(reg.has_value());
  ASSERT_EQ(reg->baskets().size(), 1u);
  ASSERT_EQ(reg->baskets()[0].members.size(), 2u);

  const auto path = testing::TempDir() + "basket.feedlog";
  {
    std::ofstream out(path);
    // First member two-sided: 40/44 (mid 42c, bid 40c). Basket incomplete.
    out << "1000\tpolymarket\t"
        << R"({"event_type":"book","asset_id":"1001","market":"0xa",)"
        << R"("bids":[{"price":"0.40","size":"10"}],)"
        << R"("asks":[{"price":"0.44","size":"10"}]})"
        << "\n";
    // Second member two-sided: 50/54 (mid 52c, bid 50c). Basket complete:
    // mid-sum $0.94, bid-sum $0.90.
    out << "2000\tpolymarket\t"
        << R"({"event_type":"book","asset_id":"1002","market":"0xb",)"
        << R"("bids":[{"price":"0.50","size":"10"}],)"
        << R"("asks":[{"price":"0.54","size":"10"}]})"
        << "\n";
    // First member's bid improves to 46: mid-sum $0.97, bid-sum $0.96.
    out << "3000\tpolymarket\t"
        << R"({"event_type":"price_change","market":"0xa","price_changes":)"
        << R"([{"asset_id":"1001","price":"0.46","side":"BUY","size":"5"}],)"
        << R"("timestamp":"1750000000500"})"
        << "\n";
    // Second member's book empties on the ask side: basket incomplete
    // again, so no further samples, but the update still counts coverage.
    out << "4000\tpolymarket\t"
        << R"({"event_type":"book","asset_id":"1002","market":"0xb",)"
        << R"("bids":[{"price":"0.50","size":"10"}],"asks":[]})"
        << "\n";
  }
  ReplayHarness harness(*reg);
  const auto stats = harness.run(path);
  ASSERT_TRUE(stats.has_value());
  ASSERT_EQ(stats->baskets.size(), 1u);
  const auto& b = stats->baskets[0];
  EXPECT_EQ(b.basket_id, "demo");
  EXPECT_EQ(b.members, 2u);
  // Kalshi never quoted: zero samples, zero coverage.
  EXPECT_EQ(b.kalshi.samples, 0u);
  EXPECT_EQ(b.kalshi.max_two_sided, 0u);
  // Polymarket sampled at ts 2000 and 3000 only.
  EXPECT_EQ(b.polymarket.samples, 2u);
  EXPECT_EQ(b.polymarket.max_two_sided, 2u);
  EXPECT_NEAR(b.polymarket.mid_sum_min_dollars, 0.94, 1e-9);
  EXPECT_NEAR(b.polymarket.mid_sum_max_dollars, 0.97, 1e-9);
  EXPECT_NEAR(b.polymarket.mid_sum_mean_dollars, (0.94 + 0.97) / 2.0, 1e-9);
  EXPECT_NEAR(b.polymarket.bid_sum_max_dollars, 0.96, 1e-9);
}

TEST(ReplayHarness, RegistrySingletonBasketsArePruned) {
  auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "solo"
basket = "lonely"
polymarket_token = "2001"

[[event]]
id = "pair-a"
basket = "pair"
polymarket_token = "2002"

[[event]]
id = "pair-b"
basket = "pair"
polymarket_token = "2003"
)");
  ASSERT_TRUE(reg.has_value());
  ASSERT_EQ(reg->baskets().size(), 1u);
  EXPECT_EQ(reg->baskets()[0].id, "pair");
}
