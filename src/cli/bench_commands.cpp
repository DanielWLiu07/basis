#include "cli/commands.h"

#include <cstdio>
#include <string>

#include "bench/fanout_bench.h"
#include "bench/lob_bench.h"
#include "cli/args.h"
#include "cli/usage.h"
#include "core/logger.h"
#include "feed/binance_parser.h"
#include "feed/coinbase_parser.h"
#include "feed/feed_log.h"
#include "feed/kalshi_parser.h"
#include "feed/polymarket_parser.h"
#include "model/order_book.h"
#include "model/unified_book.h"

namespace basis::cli {

// Matching-engine microbenchmark: replays one deterministic order flow
// through the production book and through a std::map baseline, and
// reports both. The agreement flag guards the comparison - if the two
// books produced different fills the timings describe different
// workloads, and the numbers mean nothing.
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

}  // namespace basis::cli
