#include "cli/commands.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "bench/replay_harness.h"
#include "bench/stats_report.h"
#include "bench/synth_generator.h"
#include "cli/args.h"
#include "cli/usage.h"
#include "core/counting_resource.h"
#include "core/logger.h"
#include "model/unified_book.h"
#include "normalize/contract_registry.h"

#ifdef BASIS_HAS_BDE
// Only the BDE configuration compiles BdeParseArena below, so this is the
// one include no other build can catch the loss of.
#include "alloc/bde_arena.h"
#endif

namespace basis::cli {

// Generating a synthetic capture, and replaying any capture.
//
// analysis_commands.cpp held all four capture-reading subcommands and was
// the largest file in the repo at 737 lines. They share includes and
// nothing else: one generates a capture, one replays it, one measures
// which venue's price moves first, one reconciles a rebuilt book against
// the venue's own snapshot. Splitting on that seam is what the earlier
// main.cpp split did, and for the same reason.


// --alloc count: same heap allocator, with the traffic made visible.
class CountingParseArena final : public basis::bench::ParseArena {
 public:
  std::pmr::memory_resource* resource() override { return &counting_; }
  const basis::core::CountingResource& counts() const { return counting_; }

 private:
  basis::core::CountingResource counting_;
};

#ifdef BASIS_HAS_BDE
// --alloc bde: parse transients on a sequential arena dropped per message.
class BdeParseArena final : public basis::bench::ParseArena {
 public:
  std::pmr::memory_resource* resource() override { return arena_.resource(); }
  void release() override { arena_.release(); }

 private:
  basis::alloc::BdeSequentialArena arena_;
};
#endif

// Writes a deterministic session in the venues' real wire formats with a
// cross-venue lead injected on purpose, so the pipeline can be exercised
// end to end with no network and no credentials, and the estimators can
// be checked against an answer that is known rather than assumed.
int run_synth(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string out_path(args[0]);

  basis::bench::SynthConfig config;
  const auto steps = flag_value(args, "--steps", config.steps);
  const auto lead_ms = flag_value(args, "--lead-ms",
                                  config.lead_ns / 1'000'000);
  const auto seed = flag_value(args, "--seed",
                               static_cast<std::int64_t>(config.seed));
  // Bounds double as narrowing guards for the casts below.
  if (!steps || *steps <= 0 || *steps > 100'000'000 ||
      !lead_ms || *lead_ms < -86'400'000 || *lead_ms > 86'400'000 ||
      !seed || *seed < 0 || *seed > 4'294'967'295) {
    basis::log::error("synth: bad flag value");
    return usage();
  }
  config.steps = static_cast<int>(*steps);
  config.lead_ns = *lead_ms * 1'000'000;
  config.seed = static_cast<unsigned>(*seed);

  std::string error;
  if (!basis::bench::generate_synthetic_session(config, out_path, &error)) {
    basis::log::error(error);
    return 1;
  }
  std::printf("wrote %s (%d steps, %+lld ms injected kalshi lead, seed %u)\n",
              out_path.c_str(), config.steps,
              static_cast<long long>(config.lead_ns / 1'000'000), config.seed);
  std::printf("replay it:  basis replay %s\n", out_path.c_str());
  return 0;
}

// Cross-venue lead-lag on two venues quoting the SAME instrument, from one
// capture in which both sides were stamped by the same clock. Until this
// existed the lead-lag estimator had only ever been run against a synthetic
// session with a known injected lead, which proves the estimator recovers
// what was put in but says nothing about a real market.
//
// The measurement is deliberately conservative about what it can resolve;
// see docs/bench/cross_venue_lead.md for the bias budget. Two effects push
// the estimate in a known direction and neither is a property of price
// discovery: Coinbase batches level2 updates on a 50 ms timer, and the two
// venues sit at different network distances. Both delay Coinbase relative
// to Binance, so a small positive "Binance leads" result is exactly what
// the instrument would print even if the venues moved together.
int run_replay(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string in_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/synthetic.toml");
  const auto alloc_mode = flag_string(args, "--alloc", "heap");
  const bool breakdown = has_flag(args, "--breakdown");
  const bool as_json = has_flag(args, "--json");
  const auto csv_path = flag_string(args, "--csv", "");
  // Replay at the capture's own arrival schedule, compressed by this
  // factor. 0 keeps the historical flat-out replay, which can only ever
  // report service time.
  const auto speed = flag_double(args, "--speed", 0.0);
  // How much of each pacing wait is spun instead of slept. Past the
  // capture's largest gap this spins every wait, which is what it takes
  // for the harness's own timer error to stop dominating a
  // microsecond-scale response time.
  const auto spin_ms = flag_double(args, "--pace-spin-ms", 2.0);
  const auto episodes_csv_path = flag_string(args, "--episodes-csv", "");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }

  // Allocation setup outlives the harness: books free their nodes on
  // destruction, so their resource has to be destroyed after it.
  CountingParseArena counting_arena;
  basis::core::CountingResource counting_books;
#ifdef BASIS_HAS_BDE
  BdeParseArena bde_arena;
  basis::alloc::BdeMultipool bde_books;
#endif

  basis::bench::ParseArena* parse_arena = nullptr;
  std::pmr::memory_resource* book_mr = std::pmr::get_default_resource();
  if (alloc_mode == "count") {
    parse_arena = &counting_arena;
    book_mr = &counting_books;
  } else if (alloc_mode == "bde") {
#ifdef BASIS_HAS_BDE
    parse_arena = &bde_arena;
    book_mr = bde_books.resource();
#else
    basis::log::error("--alloc bde needs a build with BASIS_ENABLE_BDE=ON");
    return 1;
#endif
  } else if (alloc_mode != "heap") {
    basis::log::error("unknown --alloc mode: " + alloc_mode);
    return usage();
  }

  basis::api::InProcessSession session;

  // --csv taps the same api-layer stream any consumer would see and writes
  // it as long-format rows (recv_ns,event_id,field,value), one per update:
  // trivially pivotable, lossless, and identical to what a subscriber gets.
  std::ofstream csv;
  if (!csv_path.empty()) {
    csv.open(csv_path, std::ios::trunc);
    if (!csv) {
      basis::log::error("cannot open --csv file: " + csv_path);
      return 1;
    }
    csv << "recv_ns,event_id,field,value\n";
    for (const auto& event_id : registry->event_ids()) {
      for (const char* field : {"kalshi_mid", "poly_mid", "basis"}) {
        session.subscribe(event_id, field,
                          [&csv, event_id, field](const basis::api::Update& u) {
                            csv << u.ts_ns << ',' << event_id << ',' << field
                                << ',' << u.value << '\n';
                          });
      }
    }
  }

  if (speed < 0.0 || (speed > 0.0 && speed < 0.001) || speed > 1e9) {
    basis::log::error("replay: --speed must be 0 (unpaced) or a positive "
                      "multiple of real time");
    return 1;
  }

  basis::bench::ReplayHarness harness(*registry, &session, {}, book_mr);
  harness.set_parse_arena(parse_arena);
  harness.set_breakdown(breakdown);
  harness.set_replay_speed(speed);
  harness.set_pace_spin_ns(static_cast<std::int64_t>(spin_ms * 1e6));
  const auto stats = harness.run(in_path, &error);
  if (!stats) {
    basis::log::error(error);
    return 1;
  }

  if (!episodes_csv_path.empty() &&
      !basis::bench::write_episodes_csv(episodes_csv_path, *stats)) {
    return 1;
  }

  // JSON mode prints one machine-readable object and nothing else, so a
  // consumer can parse stdout directly. The allocation fields are present
  // only when they were actually counted.
  if (as_json) {
    double parse_per_msg = -1.0;
    double parse_bytes_per_msg = -1.0;
    double book_per_msg = -1.0;
    if (alloc_mode == "count" && stats->records > 0) {
      const double n = static_cast<double>(stats->records);
      parse_per_msg = static_cast<double>(counting_arena.counts().allocations()) / n;
      parse_bytes_per_msg = static_cast<double>(counting_arena.counts().bytes()) / n;
      book_per_msg = static_cast<double>(counting_books.allocations()) / n;
    }
    basis::bench::print_stats_json(*stats, parse_per_msg, parse_bytes_per_msg, book_per_msg);
    return 0;
  }

  std::printf("replayed %s against %s (alloc %s)\n\n", in_path.c_str(),
              config_path.c_str(), alloc_mode.c_str());
  basis::bench::print_stats(*stats);

  std::printf("\npipeline  %.1f ms for %llu records (%.0fk records/sec)\n",
              static_cast<double>(stats->pipeline_ns) / 1e6,
              u(stats->records),
              stats->pipeline_ns > 0
                  ? static_cast<double>(stats->records) * 1e6 /
                        static_cast<double>(stats->pipeline_ns)
                  : 0.0);
  if (alloc_mode == "count" && stats->records > 0) {
    const auto& parse = counting_arena.counts();
    const double n = static_cast<double>(stats->records);
    std::printf("allocs    parse %llu (%.1f/msg, %.0f B/msg), "
                "books %llu (%.1f/msg)\n",
                u(parse.allocations()),
                static_cast<double>(parse.allocations()) / n,
                static_cast<double>(parse.bytes()) / n,
                u(counting_books.allocations()),
                static_cast<double>(counting_books.allocations()) / n);
  }
  if (breakdown) {
    const double staged = static_cast<double>(stats->parse_ns_total +
                                              stats->downstream_ns_total);
    if (staged > 0.0) {
      // The per-stage clock read inflates the total, so this run is for
      // the split, not the headline latency (which the default run gives).
      std::printf("breakdown parse %4.1f%%  downstream %4.1f%%  "
                  "(%.0f + %.0f ns/msg over %llu records)\n",
                  100.0 * static_cast<double>(stats->parse_ns_total) / staged,
                  100.0 * static_cast<double>(stats->downstream_ns_total) /
                      staged,
                  static_cast<double>(stats->parse_ns_total) /
                      static_cast<double>(stats->records),
                  static_cast<double>(stats->downstream_ns_total) /
                      static_cast<double>(stats->records),
                  u(stats->records));
    }
  }
  return 0;
}
}  // namespace basis::cli
