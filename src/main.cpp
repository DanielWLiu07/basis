#include <charconv>
#include <cstdio>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analytics/consensus.h"
#include <chrono>
#include <array>
#include <cmath>
#include <map>

#include "bench/fanout_bench.h"
#include "analytics/event_study.h"
#include "analytics/lead_lag.h"
#include "feed/binance_parser.h"
#include "feed/coinbase_parser.h"
#include "feed/book_sequencer.h"
#include "feed/feed_log.h"
#include "model/order_book.h"
#include "model/unified_book.h"
#include "bench/lob_bench.h"
#include "bench/replay_harness.h"
#include "bench/stats_report.h"
#include "bench/synth_generator.h"
#include "core/counting_resource.h"
#include "core/logger.h"
#include "core/version.h"
#include "normalize/contract_registry.h"

#ifdef BASIS_HAS_BDE
#include "alloc/bde_arena.h"
#endif

#ifdef BASIS_HAS_NET
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <csignal>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "analytics/divergence.h"
#include "analytics/lead_lag.h"
#include "core/bounded_queue.h"
#include "feed/feed_log.h"
#include "feed_live/binance_feed.h"
#include "feed_live/coinbase_feed.h"
#include "feed_live/kalshi_feed.h"
#include "feed_live/polymarket_feed.h"
#include "net/kalshi_auth.h"
#include "normalize/normalizer.h"
#endif

namespace {

// printf's %llu wants unsigned long long; our counters are uint64_t. One
// short name instead of a 40-character cast at every use site.
unsigned long long u(std::uint64_t v) {
  return static_cast<unsigned long long>(v);
}

int usage() {
  std::printf(
      "basis %s - cross-venue prediction-market data engine\n"
      "\n"
      "usage:\n"
      "  basis synth <out.feedlog> [--steps N] [--lead-ms L] [--seed S]\n"
      "      generate a deterministic synthetic session (real wire formats,\n"
      "      injected cross-venue lead; ids match configs/synthetic.toml)\n"
      "\n"
      "  basis replay <in.feedlog> [--config <contracts.toml>]\n"
      "               [--alloc heap|count|bde] [--breakdown] [--json]\n"
      "      replay a capture through parse -> normalize -> analytics -> api\n"
      "      and report basis, lead-lag, and ingest-to-signal latency\n"
      "      (default config: configs/synthetic.toml)\n"
      "      --alloc count reports heap traffic per message; --alloc bde\n"
      "      runs the hot path on Bloomberg bdlma arenas (needs a build\n"
      "      with BASIS_ENABLE_BDE); --breakdown splits latency into parse\n"
      "      vs downstream (a separate profiling run, not the headline);\n"
      "      --json prints one machine-readable object and nothing else;\n"
      "      --csv <file> writes the api-layer stream as long-format rows\n"
      "                   (recv_ns,event_id,field,value) for plotting\n"
      "      --episodes-csv <file> one row per crossed episode, incl. the\n"
      "                            surviving sweep at open/50/100/250 ms\n"
      "\n"
      "  basis book-verify <capture.feedlog> [--levels N]\n"
      "      rebuild a book from a venue diff stream through the sequencer\n"
      "      and check it against the venue's own snapshot\n"
      "\n"
      "  basis ingest-bench <capture.feedlog>\n"
      "  basis xvenue-lead <capture.feedlog> [--grid-ms N] [--max-lag-bins N]\n"
      "      parse + book-apply throughput on a captured venue feed, with\n"
      "      the venue's own message rate alongside it\n"
      "\n"
      "  basis fanout-bench [--subscribers N] [--slow K] [--updates M]\n"
      "                     [--slow-us U]\n"
      "      subscription fan-out with slow consumers: the same update\n"
      "      stream through the synchronous session and the conflating one\n"
      "\n"
      "  basis lob-bench [--ops N] [--seed S]\n"
      "      matching-engine microbenchmark: replays one deterministic order\n"
      "      flow through the price-time-priority book and through a\n"
      "      std::map baseline, and reports ns/op for both\n"
#ifdef BASIS_HAS_NET
      "\n"
      "  basis record <out.feedlog> [--config <contracts.toml>] [--seconds N]\n"
      "      [--binance sym,sym] [--coinbase PROD,PROD]  (no credentials needed)\n"
      "               [--kalshi-key-id ID] [--kalshi-pem <key.pem>]\n"
      "      capture live feeds for the configured contracts; stop with\n"
      "      --seconds or ctrl-c (default config: configs/contracts.toml).\n"
      "      Polymarket needs no credentials. Kalshi joins the capture when\n"
      "      --kalshi-key-id is given (RSA key from --kalshi-pem, default\n"
      "      secrets/kalshi.pem, never committed)\n"
      "\n"
      "  basis live [--config <contracts.toml>] [--seconds N] [--report N]\n"
      "             [--kalshi-key-id ID] [--kalshi-pem <key.pem>]\n"
      "      stream the configured contracts and print per-event basis in\n"
      "      real time: IO threads feed a bounded queue drained by one\n"
      "      analytics thread; a final report gives lead-lag and queue\n"
      "      accounting (same credential rules as record)\n"
#endif
      ,
      basis::kVersion);
  return 1;
}

std::optional<std::int64_t> parse_int(std::string_view s) {
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
  return value;
}

// Flags are --name value pairs after the positional argument.
std::optional<std::int64_t> flag_value(const std::vector<std::string_view>& args,
                                       std::string_view name,
                                       std::int64_t fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) {
      return parse_int(args[i + 1]);  // nullopt: malformed number
    }
  }
  return fallback;
}

std::string flag_string(const std::vector<std::string_view>& args,
                        std::string_view name, std::string fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return std::string(args[i + 1]);
  }
  return fallback;
}

// Presence-only flag: true if `name` appears anywhere in args.
// Comma-separated flag values (--binance btcusdt,ethusdt). Empty entries
// are dropped rather than turned into an empty subscription.
std::vector<std::string> split_csv(std::string_view text) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    auto item = text.substr(0, comma);
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return out;
}

bool has_flag(const std::vector<std::string_view>& args,
              std::string_view name) {
  for (const auto& arg : args) {
    if (arg == name) return true;
  }
  return false;
}

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

// Matching-engine microbenchmark: replays one deterministic order flow
// through the production book and through a std::map baseline, and reports
// both. The agreement flag guards the comparison - if the two books
// produced different fills, the timings describe different workloads and
// the numbers are meaningless.

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

// Writes one row per crossed run, in time order: the distribution behind
// the report's count/longest aggregates, ready for a histogram of how
// long dislocations persist and what they were worth. Sits beside the
// other two report emitters (print_stats, print_stats_json).
int run_lob_bench_cmd(const std::vector<std::string_view>& args) {
  const auto ops = flag_value(args, "--ops", 2'000'000);
  const auto seed = flag_value(args, "--seed", 7);
  if (!ops || *ops <= 0 || *ops > 200'000'000 ||
      !seed || *seed < 0 || *seed > 4'294'967'295) {
    basis::log::error("lob-bench: bad flag value");
    return usage();
  }
  const auto r = basis::bench::run_lob_bench(
      static_cast<std::uint64_t>(*ops), static_cast<std::uint32_t>(*seed));
  if (!r.agreed) {
    basis::log::error("lob-bench: ladder and map books disagreed on fills");
    return 1;
  }
  const auto pct = [](const char* name,
                      const basis::bench::LatencyPercentiles& p) {
    std::printf("LOB_LATENCY %-6s n=%-9llu p50=%.0fns p99=%.0fns "
                "p99.9=%.0fns max=%.0fns\n",
                name, u(p.samples), p.p50, p.p99, p.p999, p.max);
  };
  std::printf("LOB_BENCH ops=%llu fills=%llu filled_contracts=%lld "
              "ladder_ns_per_op=%.1f map_ns_per_op=%.1f speedup=%.2fx "
              "ladder_ops_per_sec=%.0f agreed=1\n",
              u(r.ops), u(r.fills),
              static_cast<long long>(r.filled_size),
              r.ladder_ns_per_op, r.map_ns_per_op, r.speedup,
              r.ladder_ns_per_op > 0.0 ? 1e9 / r.ladder_ns_per_op : 0.0);
  // Per-operation tail latency. Each sample includes one clock-read pair,
  // reported as timer_overhead so the numbers stay raw.
  std::printf("LOB_LATENCY clock_tick=%.0fns (per-op p50 sits on this "
              "floor; the accurate central number is the throughput mean "
              "above)\n", r.timer_overhead_ns);
  pct("rest", r.rest_latency);
  pct("cross", r.cross_latency);
  pct("cancel", r.cancel_latency);
  pct("rest*", r.rest_latency_growing);
  std::printf("LOB_LATENCY rest* is the same path on a book that was not "
              "pre-sized: container growth shows up only in the tail\n");
  std::printf("LOB_PASSIVE orders=%llu filled=%llu fill_rate=%.3f "
              "queue_ahead_median_filled=%.0f "
              "queue_ahead_median_unfilled=%.0f\n",
              u(r.passive_orders), u(r.passive_filled), r.passive_fill_rate,
              r.queue_ahead_median_filled, r.queue_ahead_median_unfilled);
  return 0;
}

// Subscription fan-out under a slow consumer: the same update stream
// through the synchronous session (handlers inline on the publisher) and
// the conflating one (publisher slots values, consumers drain).
int run_fanout_bench_cmd(const std::vector<std::string_view>& args) {
  const auto subs = flag_value(args, "--subscribers", 64);
  const auto slow = flag_value(args, "--slow", 1);
  const auto updates = flag_value(args, "--updates", 20'000);
  const auto slow_us = flag_value(args, "--slow-us", 50);
  if (!subs || *subs <= 0 || *subs > 4'096 ||
      !slow || *slow < 0 || *slow > *subs ||
      !updates || *updates <= 0 || *updates > 10'000'000 ||
      !slow_us || *slow_us < 0 || *slow_us > 100'000) {
    basis::log::error("fanout-bench: bad flag value");
    return usage();
  }
  const auto r = basis::bench::run_fanout_bench(
      static_cast<std::uint64_t>(*subs), static_cast<std::uint64_t>(*slow),
      static_cast<std::uint64_t>(*updates),
      static_cast<std::uint64_t>(*slow_us));
  std::printf("FANOUT subscribers=%llu slow=%llu slow_handler_us=%llu "
              "updates=%llu\n",
              u(r.subscribers), u(r.slow_subscribers), u(r.slow_handler_us),
              u(r.updates));
  std::printf("FANOUT sync        publish_ms=%.1f updates_per_sec=%.0f\n",
              r.sync_publish_ms, r.sync_updates_per_sec);
  std::printf("FANOUT conflating  publish_ms=%.1f updates_per_sec=%.0f "
              "speedup=%.1fx\n",
              r.conflating_publish_ms, r.conflating_updates_per_sec,
              r.speedup);
  std::printf("FANOUT delivered=%llu conflated=%llu "
              "worst_staleness_updates=%llu\n",
              u(r.delivered), u(r.conflated), u(r.worst_staleness_updates));
  return 0;
}

// Ingest throughput on a captured feed: parse + book apply, the hot path,
// timed against real venue data rather than a synthetic stream. Reports
// both the engine's rate and the venue's own message rate from the
// capture's receive timestamps, because the gap between them is the point.
int run_ingest_bench_cmd(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string path(args[0]);
  std::ifstream in(path);
  if (!in) {
    basis::log::error("ingest-bench: cannot open " + path);
    return 1;
  }
  // Load the whole capture first: this measures parsing, not file IO.
  std::vector<std::pair<std::int64_t, std::string>> records;
  std::string line;
  while (std::getline(in, line)) {
    const auto t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const auto t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const auto ts = parse_int(std::string_view(line).substr(0, t1));
    if (!ts) continue;
    records.emplace_back(*ts, line.substr(t2 + 1));
  }
  if (records.empty()) {
    basis::log::error("ingest-bench: no usable records in " + path);
    return 1;
  }

  basis::feed::BinanceParser parser;
  basis::model::UnifiedBook book;
  std::uint64_t deltas = 0, ok = 0, ignored = 0, malformed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& [recv_ns, payload] : records) {
    auto r = parser.parse(payload, recv_ns);
    switch (r.status) {
      case basis::feed::ParseStatus::Ok: ++ok; break;
      case basis::feed::ParseStatus::Ignored: ++ignored; break;
      case basis::feed::ParseStatus::Malformed: ++malformed; break;
    }
    for (const auto& d : r.deltas) {
      book.apply(d);
      ++deltas;
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0).count();

  const double venue_span_s =
      static_cast<double>(records.back().first - records.front().first) / 1e9;
  const double engine_rate = ms > 0.0 ? records.size() * 1000.0 / ms : 0.0;
  const double venue_rate =
      venue_span_s > 0.0 ? records.size() / venue_span_s : 0.0;
  std::printf("INGEST records=%llu ok=%llu ignored=%llu malformed=%llu "
              "deltas=%llu\n",
              u(records.size()), u(ok), u(ignored), u(malformed), u(deltas));
  std::printf("INGEST venue_span_s=%.1f venue_msgs_per_sec=%.0f\n",
              venue_span_s, venue_rate);
  std::printf("INGEST engine_ms=%.1f engine_msgs_per_sec=%.0f "
              "engine_deltas_per_sec=%.0f headroom=%.0fx\n",
              ms, engine_rate,
              ms > 0.0 ? deltas * 1000.0 / ms : 0.0,
              venue_rate > 0.0 ? engine_rate / venue_rate : 0.0);
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
int run_xvenue_lead_cmd(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string path(args[0]);
  std::int64_t grid_ms = 50;
  int max_lag_bins = 100;
  // A "repricing" has to be scaled to the instrument. The estimator's
  // default is 1 cent, which is the right bar for a contract that trades
  // between 1c and 99c and pure noise on one quoting around $63,000; a
  // dollar is roughly one tick of genuine movement in BTC.
  std::int64_t move_cents = 100;
  std::int64_t follow_ms = 2000;
  // Observe both venues on one clock rather than on every message.
  //
  // This is not a tuning knob, it corrects a bias. Binance pushes on every
  // change of the touch while Coinbase coalesces on a 50 ms timer, so
  // sampling per message compares Binance's many small steps against
  // Coinbase's few coarse jumps. A fixed "a repricing is N cents" bar then
  // catches most of Coinbase's jumps and misses Binance's gradual moves,
  // and the event study reads that as Coinbase moving far more often --
  // an artifact of update granularity that looks exactly like Coinbase
  // being the noisy follower. Sampling both on the same grid with the last
  // known mid measures each venue's movement over identical windows.
  // 0 restores per-message sampling.
  std::int64_t sample_ms = 50;
  for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
    const auto v = parse_int(args[i + 1]);
    if (!v) continue;
    if (args[i] == "--grid-ms") grid_ms = *v;
    else if (args[i] == "--max-lag-bins") max_lag_bins = static_cast<int>(*v);
    else if (args[i] == "--move-cents") move_cents = *v;
    else if (args[i] == "--follow-ms") follow_ms = *v;
    else if (args[i] == "--sample-ms") sample_ms = *v;
  }
  if (grid_ms <= 0 || max_lag_bins <= 0) {
    basis::log::error("xvenue-lead: grid-ms and max-lag-bins must be positive");
    return 1;
  }

  basis::feed::FeedLogReader reader(path);
  if (!reader.ok()) {
    basis::log::error("xvenue-lead: cannot open " + path);
    return 1;
  }

  basis::feed::BinanceParser binance;
  basis::feed::CoinbaseParser coinbase;
  // One book per venue. The capture carries exactly one product per venue;
  // that is asserted below rather than assumed, because silently averaging
  // two instruments into one mid would produce a plausible-looking number
  // out of nonsense.
  std::array<basis::model::OrderBook, basis::model::kVenueCount> books;
  std::array<std::string, basis::model::kVenueCount> markets;
  std::array<std::uint64_t, basis::model::kVenueCount> msgs{};
  bool mixed_markets = false;

  basis::analytics::LeadLagConfig cfg;
  cfg.grid_ns = grid_ms * 1'000'000;
  cfg.max_lag_bins = max_lag_bins;
  basis::analytics::CrossCorrelationEstimator est(cfg);
  // The cross-correlation estimator resamples onto a fixed grid, so it can
  // never resolve a lead shorter than one bin. The event study works on the
  // raw irregular timestamps instead and answers a different question --
  // whose moves get answered, and how fast -- so it is not bounded by the
  // grid. Running both is the point: they fail in different ways.
  basis::analytics::EventStudyEstimator ev(
      {.move_cents = static_cast<double>(move_cents),
       .follow_window_ns = follow_ms * 1'000'000});

  std::uint64_t records = 0, malformed = 0, observations = 0;
  std::uint64_t unrepresentable = 0;
  std::int64_t next_sample_ns = 0;
  std::int64_t first_ns = 0, last_ns = 0;
  while (auto rec = reader.next()) {
    ++records;
    if (first_ns == 0) first_ns = rec->recv_ns;
    last_ns = rec->recv_ns;
    const auto vi = static_cast<std::size_t>(rec->venue);
    basis::feed::ParseResult r =
        rec->venue == basis::model::Venue::Coinbase
            ? coinbase.parse(rec->payload, rec->recv_ns)
            : binance.parse(rec->payload, rec->recv_ns);
    unrepresentable += r.levels_unrepresentable;
    if (r.status == basis::feed::ParseStatus::Malformed) { ++malformed; continue; }
    if (r.status != basis::feed::ParseStatus::Ok) continue;
    ++msgs[vi];
    for (const auto& d : r.deltas) {
      if (markets[vi].empty()) markets[vi] = std::string(d.market);
      else if (markets[vi] != d.market) mixed_markets = true;
      books[vi].apply(d);
    }
    // Sample whenever either venue moves, but only once both are two-sided;
    // the estimator resamples onto its own grid, so an uneven arrival rate
    // between the venues is handled there rather than by dropping data.
    const auto a = books[static_cast<std::size_t>(basis::model::Venue::Binance)].mid();
    const auto b = books[static_cast<std::size_t>(basis::model::Venue::Coinbase)].mid();
    if (a && b) {
      if (sample_ms <= 0) {
        est.observe(*a, *b, rec->recv_ns);
        ev.observe(*a, *b, rec->recv_ns);
        ++observations;
      } else {
        const std::int64_t step = sample_ms * 1'000'000;
        if (next_sample_ns == 0) next_sample_ns = rec->recv_ns;
        while (rec->recv_ns >= next_sample_ns) {
          est.observe(*a, *b, next_sample_ns);
          ev.observe(*a, *b, next_sample_ns);
          ++observations;
          next_sample_ns += step;
        }
      }
    }
  }

  if (mixed_markets) {
    basis::log::error("xvenue-lead: a venue quoted more than one product; "
                      "the mid would mix instruments");
    return 1;
  }
  const auto bi = static_cast<std::size_t>(basis::model::Venue::Binance);
  const auto ci = static_cast<std::size_t>(basis::model::Venue::Coinbase);
  if (msgs[bi] == 0 || msgs[ci] == 0) {
    basis::log::error("xvenue-lead: capture does not carry both venues");
    return 1;
  }

  const double span_s = static_cast<double>(last_ns - first_ns) / 1e9;
  const auto res = est.estimate();
  std::printf("XVENUE span_s=%.1f records=%llu malformed=%llu "
              "unrepresentable_levels=%llu binance_msgs=%llu "
              "coinbase_msgs=%llu observations=%llu\n",
              span_s, u(records), u(malformed), u(unrepresentable),
              u(msgs[bi]), u(msgs[ci]), u(observations));
  std::printf("XVENUE binance_market=%s coinbase_market=%s "
              "grid_ms=%lld max_lag_bins=%d sample_ms=%lld\n",
              markets[bi].c_str(), markets[ci].c_str(),
              static_cast<long long>(grid_ms), max_lag_bins,
              static_cast<long long>(sample_ms));
  std::printf("XVENUE lead_ms=%.1f corr=%.4f samples=%llu "
              "ci_low_ms=%.1f ci_high_ms=%.1f resamples=%llu significant=%d\n",
              res.lead_seconds * 1000.0, res.correlation, u(res.samples),
              res.ci_low_seconds * 1000.0, res.ci_high_seconds * 1000.0,
              u(res.resamples), res.lead_is_significant() ? 1 : 0);
  const auto evr = ev.estimate();
  std::printf("XVENUE event_move_cents=%lld follow_window_ms=%lld\n",
              static_cast<long long>(move_cents),
              static_cast<long long>(follow_ms));
  std::printf("XVENUE binance_moves=%llu answered=%llu rate=%.3f "
              "median_follow_ms=%.1f\n",
              u(evr.moves), u(evr.followed), evr.forward_follow_rate(),
              evr.median_follow_seconds * 1000.0);
  std::printf("XVENUE coinbase_moves=%llu answered=%llu rate=%.3f "
              "median_follow_ms=%.1f\n",
              u(evr.reverse_moves), u(evr.reverse_followed),
              evr.reverse_follow_rate(),
              evr.reverse_median_follow_seconds * 1000.0);
  std::printf("XVENUE follow_rate_z=%.2f confirmed_leader=%d\n",
              evr.follow_rate_z(), evr.confirmed_leader());
  std::printf("XVENUE note=positive_lead_means_binance_leads_coinbase\n");
  return 0;
}

// Reconstructs a book from a venue diff stream through the sequencer and
// checks it against a snapshot the venue produced independently. The
// capture interleaves three record kinds: diff, snapshot (the one joined
// from), and validation_snapshot (the one checked against, taken later so
// the stream runs past it).
int run_book_verify_cmd(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string path(args[0]);
  const auto depth = flag_value(args, "--levels", 20);
  if (!depth || *depth <= 0 || *depth > 1000) {
    basis::log::error("book-verify: bad --levels");
    return usage();
  }
  std::ifstream in(path);
  if (!in) {
    basis::log::error("book-verify: cannot open " + path);
    return 1;
  }

  simdjson::dom::parser parser;
  basis::feed::BookSequencer seq;
  // price cents -> size, mirroring the venue's own book.
  std::map<std::int64_t, std::string, std::greater<std::int64_t>> bids;
  std::map<std::int64_t, std::string> asks;
  std::string validation;
  std::int64_t target = -1;
  std::int64_t final_u = -1;
  bool reached = false;

  const auto to_cents = [](std::string_view s) {
    return static_cast<std::int64_t>(std::llround(std::strtod(
        std::string(s).c_str(), nullptr) * 100.0));
  };
  // Every level is [price, qty]; a shorter array is malformed input, not
  // a reason to read past the end. binance_parser.cpp guards the same
  // shape and this had not carried the guard over.
  const auto load_side = [&](simdjson::dom::element levels, bool bid) -> bool {
    simdjson::dom::array arr;
    if (levels.get_array().get(arr) != simdjson::SUCCESS) return false;
    for (auto lv : arr) {
      simdjson::dom::array pair;
      if (lv.get_array().get(pair) != simdjson::SUCCESS) return false;
      auto it = pair.begin();
      if (it == pair.end()) return false;
      std::string_view px;
      if ((*it).get_string().get(px) != simdjson::SUCCESS) return false;
      ++it;
      if (it == pair.end()) return false;
      std::string_view qty;
      if ((*it).get_string().get(qty) != simdjson::SUCCESS) return false;
      const std::int64_t cents = to_cents(px);
      const bool empty = std::strtod(std::string(qty).c_str(), nullptr) == 0.0;
      if (bid) {
        if (empty) bids.erase(cents); else bids[cents] = std::string(qty);
      } else {
        if (empty) asks.erase(cents); else asks[cents] = std::string(qty);
      }
    }
    return true;
  };
  const auto fail = [](const char* why) {
    basis::log::error(std::string("book-verify: ") + why);
    return 1;
  };

  std::string line;
  while (std::getline(in, line)) {
    const auto t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const auto t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const std::string kind = line.substr(t1 + 1, t2 - t1 - 1);
    const std::string payload = line.substr(t2 + 1);
    simdjson::dom::element doc;
    if (parser.parse(simdjson::padded_string(payload)).get(doc) !=
        simdjson::SUCCESS) {
      continue;
    }
    if (kind == "snapshot") {
      std::int64_t last = 0;
      if (doc["lastUpdateId"].get_int64().get(last) != simdjson::SUCCESS) {
        return fail("snapshot has no lastUpdateId");
      }
      bids.clear();
      asks.clear();
      if (!load_side(doc["bids"], true) || !load_side(doc["asks"], false)) {
        return fail("snapshot levels malformed");
      }
      seq.on_snapshot(last);
    } else if (kind == "validation_snapshot") {
      if (doc["lastUpdateId"].get_int64().get(target) != simdjson::SUCCESS) {
        return fail("validation snapshot has no lastUpdateId");
      }
      validation = payload;
    } else if (kind == "diff" && !reached) {
      std::int64_t U = 0, u = 0;
      if (doc["U"].get_int64().get(U) != simdjson::SUCCESS ||
          doc["u"].get_int64().get(u) != simdjson::SUCCESS) {
        return fail("depth event has no update-id range");
      }
      switch (seq.on_update(U, u)) {
        case basis::feed::BookSequencer::Decision::Apply:
          if (!load_side(doc["b"], true) || !load_side(doc["a"], false)) {
            return fail("depth event levels malformed");
          }
          if (target >= 0 && u >= target) {
            reached = true;
            final_u = u;
          }
          break;
        case basis::feed::BookSequencer::Decision::Gap:
          // A real feed would refetch here. In a verification run a gap
          // means the answer is "cannot verify", not a silent retry.
          break;
        default:
          break;
      }
    }
  }

  const auto& st = seq.stats();
  // final_u == target means the reconstruction stopped exactly on the
  // validation snapshot's sequence point and the comparison below is
  // exact. Depth events are atomic, so a snapshot taken strictly inside
  // an event's range leaves the book a few ids past it; the overshoot is
  // reported rather than hidden, because a clean result at overshoot > 0
  // is weaker evidence than one at overshoot == 0 and a reader cannot
  // tell them apart otherwise.
  const std::int64_t overshoot =
      (reached && target >= 0) ? final_u - target : -1;
  std::printf("BOOK_VERIFY applied=%llu discarded=%llu buffered=%llu "
              "gaps=%llu stale_snapshots=%llu reached_target=%d "
              "final_u=%lld target=%lld overshoot=%lld\n",
              u(st.applied), u(st.discarded), u(st.buffered), u(st.gaps),
              u(st.stale_snapshots), reached ? 1 : 0,
              static_cast<long long>(final_u),
              static_cast<long long>(target),
              static_cast<long long>(overshoot));
  if (validation.empty() || !reached) {
    basis::log::error("book-verify: never reached the validation point");
    return 1;
  }

  simdjson::dom::element vdoc;
  if (parser.parse(simdjson::padded_string(validation)).get(vdoc) !=
      simdjson::SUCCESS) {
    basis::log::error("book-verify: validation snapshot unparseable");
    return 1;
  }
  int mismatches = 0, compared = 0;
  const auto compare = [&](const char* key, bool bid) {
    int i = 0;
    auto mine_bid = bids.begin();
    auto mine_ask = asks.begin();
    for (auto lv : simdjson::dom::array(vdoc[key])) {
      if (i++ >= *depth) break;
      auto it = simdjson::dom::array(lv).begin();
      const std::int64_t vpx = to_cents((*it).get_string().value());
      ++it;
      const double vqty = std::strtod(
          std::string((*it).get_string().value()).c_str(), nullptr);
      std::int64_t mpx = 0;
      double mqty = 0.0;
      if (bid) {
        if (mine_bid == bids.end()) { ++mismatches; continue; }
        mpx = mine_bid->first;
        mqty = std::strtod(mine_bid->second.c_str(), nullptr);
        ++mine_bid;
      } else {
        if (mine_ask == asks.end()) { ++mismatches; continue; }
        mpx = mine_ask->first;
        mqty = std::strtod(mine_ask->second.c_str(), nullptr);
        ++mine_ask;
      }
      ++compared;
      if (mpx != vpx || std::fabs(mqty - vqty) > 1e-9) ++mismatches;
    }
  };
  compare("bids", true);
  compare("asks", false);
  std::printf("BOOK_VERIFY levels_compared=%d mismatches=%d %s\n",
              compared, mismatches, mismatches == 0 ? "exact" : "DIVERGED");
  return mismatches == 0 ? 0 : 1;
}

int run_replay(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string in_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/synthetic.toml");
  const auto alloc_mode = flag_string(args, "--alloc", "heap");
  const bool breakdown = has_flag(args, "--breakdown");
  const bool as_json = has_flag(args, "--json");
  const auto csv_path = flag_string(args, "--csv", "");
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

  basis::bench::ReplayHarness harness(*registry, &session, {}, book_mr);
  harness.set_parse_arena(parse_arena);
  harness.set_breakdown(breakdown);
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

#ifdef BASIS_HAS_NET

std::atomic<bool> g_interrupted{false};

void handle_sigint(int) { g_interrupted.store(true); }

int run_record(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string out_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/contracts.toml");
  const auto seconds = flag_value(args, "--seconds", 0);  // 0: until ctrl-c
  if (!seconds || *seconds < 0 || *seconds > 7 * 86'400) {
    basis::log::error("record: bad --seconds value");
    return usage();
  }
  const auto binance_symbols = split_csv(flag_string(args, "--binance", ""));
  const auto coinbase_products =
      split_csv(flag_string(args, "--coinbase", ""));
  const bool crypto_only = !binance_symbols.empty() || !coinbase_products.empty();
  const auto kalshi_key_id = flag_string(args, "--kalshi-key-id", "");
  const auto kalshi_pem =
      flag_string(args, "--kalshi-pem", "secrets/kalshi.pem");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }
  const auto& tokens = registry->polymarket_tokens();
  const auto& tickers = registry->kalshi_tickers();
  if (tokens.empty() && !crypto_only) {
    basis::log::error("no polymarket tokens in " + config_path +
                      "; nothing to record");
    return 1;
  }

  basis::feed::FeedLogWriter writer(out_path);
  if (!writer.ok()) {
    basis::log::error("cannot open " + out_path + " for writing");
    return 1;
  }

  // The raw taps run on each feed's IO thread; the mutex serializes the
  // two writers into one feedlog. Rejected writes are counted, per the
  // no-silent-drop rule.
  std::mutex writer_mutex;
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> rejected{0};
  const auto make_tap = [&](basis::model::Venue venue) {
    return [&, venue](std::string_view payload, std::int64_t recv_ns) {
      std::string framed(payload);
      // Some messages arrive with embedded newlines; outside JSON strings
      // whitespace is insignificant, so flattening keeps the payload valid
      // instead of losing a real message to the line framing.
      for (char& c : framed) {
        if (c == '\n' || c == '\r') c = ' ';
      }
      bool ok = false;
      {
        const std::lock_guard<std::mutex> lock(writer_mutex);
        ok = writer.write({recv_ns, venue, std::move(framed)});
      }
      (ok ? written : rejected).fetch_add(1);
    };
  };

  // Every venue is optional and independent; the recorder captures
  // whatever was asked for. Crypto needs no credentials and no registry
  // entry, which is what makes a cross-venue capture reproducible by
  // anyone who clones this.
  // Naming a venue on the command line means recording that venue, so the
  // registry-driven ones step aside rather than adding an unasked-for TLS
  // connection and unrelated traffic to the capture.
  std::unique_ptr<basis::feed::PolymarketFeed> poly_feed;
  if (!tokens.empty() && !crypto_only) {
    poly_feed = std::make_unique<basis::feed::PolymarketFeed>(
        basis::feed::PolymarketFeed::Config{.token_ids = tokens});
    poly_feed->set_raw_tap(make_tap(basis::model::Venue::Polymarket));
  }

  std::unique_ptr<basis::feed::BinanceFeed> binance_feed;
  if (!binance_symbols.empty()) {
    binance_feed = std::make_unique<basis::feed::BinanceFeed>(
        basis::feed::BinanceFeed::Config{.symbols = binance_symbols});
    binance_feed->set_raw_tap(make_tap(basis::model::Venue::Binance));
  }

  std::unique_ptr<basis::feed::CoinbaseFeed> coinbase_feed;
  if (!coinbase_products.empty()) {
    coinbase_feed = std::make_unique<basis::feed::CoinbaseFeed>(
        basis::feed::CoinbaseFeed::Config{.product_ids = coinbase_products});
    coinbase_feed->set_raw_tap(make_tap(basis::model::Venue::Coinbase));
  }

  // Kalshi requires an authenticated session even for market data; without
  // credentials the recording is Polymarket-only, stated up front rather
  // than discovered in the replay.
  std::unique_ptr<basis::feed::KalshiFeed> kalshi_feed;
  if (!kalshi_key_id.empty() && !crypto_only) {
    auto signer = basis::net::KalshiSigner::load(kalshi_pem, &error);
    if (!signer) {
      basis::log::error(error);
      return 1;
    }
    if (tickers.empty()) {
      basis::log::error("no kalshi tickers in " + config_path);
      return 1;
    }
    kalshi_feed = std::make_unique<basis::feed::KalshiFeed>(
        basis::feed::KalshiFeed::Config{.market_tickers = tickers,
                                        .key_id = kalshi_key_id,
                                        .signer = std::move(*signer)});
    kalshi_feed->set_raw_tap(make_tap(basis::model::Venue::Kalshi));
  }

  std::signal(SIGINT, handle_sigint);
  std::string what;
  const auto add_what = [&what](std::size_t n, const char* label) {
    if (n == 0) return;
    if (!what.empty()) what += " + ";
    what += std::to_string(n);
    what += ' ';
    what += label;
  };
  add_what(poly_feed ? tokens.size() : 0, "polymarket token(s)");
  add_what(kalshi_feed ? tickers.size() : 0, "kalshi ticker(s)");
  add_what(binance_symbols.size(), "binance symbol(s)");
  add_what(coinbase_products.size(), "coinbase product(s)");
  std::printf("recording %s -> %s%s\n", what.c_str(), out_path.c_str(),
              *seconds > 0 ? "" : "  (ctrl-c to stop)");
  // All venues are stamped by one process reading every socket, so the
  // streams share a clock. Two recorders, or two clocks, would put an
  // unknown offset straight into any cross-venue timing measured from the
  // capture (docs/bench/cross_venue_lead.md).
  if (poly_feed) poly_feed->start();
  if (kalshi_feed) kalshi_feed->start();
  if (binance_feed) binance_feed->start();
  if (coinbase_feed) coinbase_feed->start();

  const auto started = std::chrono::steady_clock::now();
  auto next_report = started + std::chrono::seconds(5);
  while (!g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (*seconds > 0 && now - started >= std::chrono::seconds(*seconds)) {
      break;
    }
    if (now >= next_report) {
      next_report += std::chrono::seconds(5);
      if (poly_feed) {
        std::printf("  poly %llu msgs %llu deltas %llu malformed %llu recon",
                    u(poly_feed->messages()),
                    u(poly_feed->deltas()),
                    u(poly_feed->malformed()),
                    u(poly_feed->reconnects()));
      }
      if (binance_feed) {
        std::printf("  |  binance %llu msgs %llu deltas %llu malformed",
                    u(binance_feed->messages()),
                    u(binance_feed->deltas()),
                    u(binance_feed->malformed()));
      }
      if (coinbase_feed) {
        std::printf("  |  coinbase %llu msgs %llu deltas %llu malformed",
                    u(coinbase_feed->messages()),
                    u(coinbase_feed->deltas()),
                    u(coinbase_feed->malformed()));
      }
      if (kalshi_feed) {
        std::printf("  |  kalshi %llu msgs %llu deltas %llu gaps %llu recon",
                    u(kalshi_feed->messages()),
                    u(kalshi_feed->deltas()),
                    u(kalshi_feed->gaps()),
                    u(
                        kalshi_feed->reconnects()));
      }
      std::printf("\n");
    }
  }
  if (poly_feed) poly_feed->stop();
  if (kalshi_feed) kalshi_feed->stop();
  if (binance_feed) binance_feed->stop();
  if (coinbase_feed) coinbase_feed->stop();

  std::printf("wrote %llu records to %s (%llu rejected)\n",
              u(written.load()),
              out_path.c_str(),
              u(rejected.load()));
  if (poly_feed) {
    std::printf("  polymarket: %llu malformed, %llu reconnects, "
                "%llu hashes verified, %llu mismatched\n",
                u(poly_feed->malformed()),
                u(poly_feed->reconnects()),
                u(poly_feed->hashes_verified()),
                u(poly_feed->hashes_mismatched()));
  }
  if (binance_feed) {
    std::printf("  binance: %llu malformed, %llu reconnects\n",
                u(binance_feed->malformed()),
                u(binance_feed->reconnects()));
  }
  if (coinbase_feed) {
    std::printf("  coinbase: %llu malformed, %llu reconnects, "
                "%llu levels unrepresentable\n",
                u(coinbase_feed->malformed()),
                u(coinbase_feed->reconnects()),
                u(coinbase_feed->unrepresentable()));
  }
  if (kalshi_feed) {
    std::printf("  kalshi: %llu malformed, %llu gaps, %llu reconnects\n",
                u(kalshi_feed->malformed()),
                u(kalshi_feed->gaps()),
                u(kalshi_feed->reconnects()));
  }
  std::printf("replay it:  basis replay %s --config %s\n", out_path.c_str(),
              config_path.c_str());
  return 0;
}

// The live pipeline's analytics half: everything downstream of the queue,
// owned by one analytics thread, with a mutex only for the periodic
// console snapshot taken by the main thread.
class LiveAnalytics {
 public:
  explicit LiveAnalytics(const basis::normalize::ContractRegistry& registry)
      : normalizer_(registry) {
    normalizer_.set_observer([this](const std::string& event_id,
                                    const basis::model::UnifiedBook& book,
                                    const basis::model::BookDelta& delta) {
      const auto kalshi_mid = book.mid(basis::model::Venue::Kalshi);
      const auto poly_mid = book.mid(basis::model::Venue::Polymarket);
      auto& ev = events_[event_id];
      ev.kalshi_mid = kalshi_mid;
      ev.poly_mid = poly_mid;
      if (kalshi_mid && poly_mid) {
        ev.divergence.observe(*kalshi_mid - *poly_mid);
        ev.lead_lag.observe(*kalshi_mid, *poly_mid, delta.ts_ns);
      }
    });
  }

  void on_delta(const basis::model::OwnedBookDelta& owned) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++deltas_;
    normalizer_.on_delta(owned.view());
  }

  void print_snapshot() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::printf("-- %llu deltas, %llu unmapped\n",
                u(deltas_),
                u(
                    normalizer_.unmapped_deltas()));
    for (const auto& [event_id, ev] : events_) {
      if (ev.kalshi_mid && ev.poly_mid) {
        std::printf("   %-28s kalshi %5.1fc  poly %5.1fc  basis %+5.1fc\n",
                    event_id.c_str(), *ev.kalshi_mid, *ev.poly_mid,
                    *ev.kalshi_mid - *ev.poly_mid);
      } else if (ev.poly_mid) {
        std::printf("   %-28s poly %5.1fc  (no kalshi book yet)\n",
                    event_id.c_str(), *ev.poly_mid);
      } else if (ev.kalshi_mid) {
        std::printf("   %-28s kalshi %5.1fc  (no polymarket book yet)\n",
                    event_id.c_str(), *ev.kalshi_mid);
      }
    }
  }

  void print_final_report() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [event_id, ev] : events_) {
      std::printf("\nevent %s\n", event_id.c_str());
      if (ev.divergence.samples() == 0) {
        std::printf("  basis    no overlap (one venue never had a "
                    "two-sided book)\n");
        continue;
      }
      std::printf("  basis    mean %+.2fc  min %+.2fc  max %+.2fc  "
                  "last %+.2fc  (%llu samples)\n",
                  ev.divergence.mean(), ev.divergence.min(),
                  ev.divergence.max(), ev.divergence.last(),
                  u(ev.divergence.samples()));
      const auto ll = ev.lead_lag.estimate();
      if (ll.correlation > 0.0) {
        const char* leader = ll.lead_seconds >= 0 ? "kalshi" : "polymarket";
        std::printf("  lead-lag %s leads by %.3fs  (corr %.2f over %llu "
                    "samples)\n",
                    leader,
                    ll.lead_seconds >= 0 ? ll.lead_seconds : -ll.lead_seconds,
                    ll.correlation,
                    u(ll.samples));
      }
    }
  }

 private:
  struct EventState {
    std::optional<double> kalshi_mid;
    std::optional<double> poly_mid;
    basis::analytics::DivergenceTracker divergence;
    basis::analytics::CrossCorrelationEstimator lead_lag;
  };

  std::mutex mutex_;
  basis::normalize::Normalizer normalizer_;
  std::map<std::string, EventState> events_;
  std::uint64_t deltas_ = 0;
};

int run_live(const std::vector<std::string_view>& args) {
  const auto config_path =
      flag_string(args, "--config", "configs/contracts.toml");
  const auto seconds = flag_value(args, "--seconds", 0);  // 0: until ctrl-c
  const auto report_every = flag_value(args, "--report", 5);
  if (!seconds || *seconds < 0 || !report_every || *report_every < 1) {
    basis::log::error("live: bad --seconds or --report value");
    return usage();
  }
  const auto kalshi_key_id = flag_string(args, "--kalshi-key-id", "");
  const auto kalshi_pem =
      flag_string(args, "--kalshi-pem", "secrets/kalshi.pem");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }

  // The one thread crossing in the engine: feed IO threads produce owned
  // deltas (a queued view into the parser buffer would dangle), one
  // analytics thread consumes. Bounded and blocking, so bursts back up
  // into TCP instead of dropping; the final report proves the accounting.
  basis::core::BoundedQueue<basis::model::OwnedBookDelta> queue(8192);
  const auto sink = [&queue](const basis::model::BookDelta& delta) {
    queue.push(basis::model::OwnedBookDelta(delta));
  };

  basis::feed::PolymarketFeed poly_feed(
      {.token_ids = registry->polymarket_tokens()});
  poly_feed.set_sink(sink);

  std::unique_ptr<basis::feed::KalshiFeed> kalshi_feed;
  if (!kalshi_key_id.empty()) {
    auto signer = basis::net::KalshiSigner::load(kalshi_pem, &error);
    if (!signer) {
      basis::log::error(error);
      return 1;
    }
    kalshi_feed = std::make_unique<basis::feed::KalshiFeed>(
        basis::feed::KalshiFeed::Config{
            .market_tickers = registry->kalshi_tickers(),
            .key_id = kalshi_key_id,
            .signer = std::move(*signer)});
    kalshi_feed->set_sink(sink);
  } else {
    std::printf("no kalshi credentials: streaming polymarket only\n");
  }

  LiveAnalytics analytics(*registry);
  std::thread analytics_thread([&] {
    while (auto owned = queue.pop()) {
      analytics.on_delta(*owned);
    }
  });

  std::signal(SIGINT, handle_sigint);
  poly_feed.start();
  if (kalshi_feed) kalshi_feed->start();

  const auto started = std::chrono::steady_clock::now();
  auto next_report = started + std::chrono::seconds(*report_every);
  while (!g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (*seconds > 0 && now - started >= std::chrono::seconds(*seconds)) {
      break;
    }
    if (now >= next_report) {
      next_report += std::chrono::seconds(*report_every);
      analytics.print_snapshot();
    }
  }

  poly_feed.stop();
  if (kalshi_feed) kalshi_feed->stop();
  queue.close();  // analytics drains what is queued, then exits
  analytics_thread.join();

  analytics.print_final_report();
  std::printf("\nqueue     %llu in, %llu out, high water %zu, "
              "%llu blocked pushes\n",
              u(queue.pushed()),
              u(queue.popped()),
              queue.high_water(),
              u(queue.blocked_pushes()));
  std::printf("feeds     poly %llu msgs %llu malformed %llu reconnects "
              "%llu hashes ok %llu mismatched",
              u(poly_feed.messages()),
              u(poly_feed.malformed()),
              u(poly_feed.reconnects()),
              u(poly_feed.hashes_verified()),
              u(
                  poly_feed.hashes_mismatched()));
  if (kalshi_feed) {
    std::printf("  |  kalshi %llu msgs %llu malformed %llu gaps "
                "%llu reconnects",
                u(kalshi_feed->messages()),
                u(kalshi_feed->malformed()),
                u(kalshi_feed->gaps()),
                u(kalshi_feed->reconnects()));
  }
  std::printf("\n");
  return 0;
}

#endif  // BASIS_HAS_NET

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty()) return usage();

  const auto command = args[0];
  const std::vector<std::string_view> rest(args.begin() + 1, args.end());
  if (command == "lob-bench") return run_lob_bench_cmd(rest);
  if (command == "fanout-bench") return run_fanout_bench_cmd(rest);
  if (command == "ingest-bench") return run_ingest_bench_cmd(rest);
  if (command == "xvenue-lead") return run_xvenue_lead_cmd(rest);
  if (command == "book-verify") return run_book_verify_cmd(rest);
  if (command == "synth") return run_synth(rest);
  if (command == "replay") return run_replay(rest);
#ifdef BASIS_HAS_NET
  if (command == "record") return run_record(rest);
  if (command == "live") return run_live(rest);
#endif
  return usage();
}
