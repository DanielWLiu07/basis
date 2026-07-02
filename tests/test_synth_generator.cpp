#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "bench/replay_harness.h"
#include "bench/synth_generator.h"
#include "normalize/contract_registry.h"

using basis::bench::ReplayHarness;
using basis::bench::SynthConfig;
using basis::bench::generate_synthetic_session;
using basis::normalize::TomlContractRegistry;

namespace {

TomlContractRegistry synthetic_registry() {
  auto reg = TomlContractRegistry::parse(R"(
[[event]]
id = "synthetic-demo"
kalshi = "SYN-DEMO"
polymarket_token = "1000001"
)");
  return *reg;
}

}  // namespace

// The closed loop that validates the whole engine: generate a session with
// a known injected lead, replay it through the real parsers and analytics,
// and require the injected number back out.
TEST(SynthGenerator, ReplayRecoversTheInjectedLead) {
  const auto path = testing::TempDir() + "synthetic.feedlog";
  SynthConfig config;
  config.steps = 3000;
  config.lead_ns = 400'000'000;  // Kalshi leads by 400 ms

  std::string error;
  ASSERT_TRUE(generate_synthetic_session(config, path, &error)) << error;

  const auto reg = synthetic_registry();
  ReplayHarness harness(reg);
  const auto stats = harness.run(path, &error);
  ASSERT_TRUE(stats.has_value()) << error;

  // A synthetic session is perfectly clean by construction.
  EXPECT_EQ(stats->malformed, 0u);
  EXPECT_EQ(stats->malformed_lines, 0u);
  EXPECT_EQ(stats->gaps, 0u);
  EXPECT_EQ(stats->unmapped_deltas, 0u);
  EXPECT_GT(stats->records, 1000u);

  ASSERT_EQ(stats->events.size(), 1u);
  const auto& event = stats->events[0];
  EXPECT_EQ(event.event_id, "synthetic-demo");
  EXPECT_GT(event.basis_samples, 1000u);

  // The estimator sees the lead through real wire formats, book folding,
  // and jittered arrival times; one grid bin (100 ms) of slack.
  EXPECT_NEAR(event.lead_lag.lead_seconds, 0.4, 0.11);
  EXPECT_GT(event.lead_lag.correlation, 0.5);
}

TEST(SynthGenerator, SameSeedSameFile) {
  const auto path_a = testing::TempDir() + "synth_a.feedlog";
  const auto path_b = testing::TempDir() + "synth_b.feedlog";
  SynthConfig config;
  config.steps = 200;

  ASSERT_TRUE(generate_synthetic_session(config, path_a));
  ASSERT_TRUE(generate_synthetic_session(config, path_b));

  std::ifstream a(path_a);
  std::ifstream b(path_b);
  const std::string text_a((std::istreambuf_iterator<char>(a)),
                           std::istreambuf_iterator<char>());
  const std::string text_b((std::istreambuf_iterator<char>(b)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(text_a.empty());
  EXPECT_EQ(text_a, text_b);
}
