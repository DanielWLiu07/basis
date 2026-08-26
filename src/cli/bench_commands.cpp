#include "cli/commands.h"

#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
//
// Each record is parsed by the parser its own venue column names, which is
// the dispatch `replay` already did. This command used to assume Binance
// for every record instead. That is correct on a Binance capture and
// silently wrong on any other: run against the committed 30 minute
// Polymarket capture it parsed all 34,731 messages as Binance, called
// every one of them Ignored, applied nothing to a book, and still printed
// `headroom=125717x` - a timed loop measuring simdjson rejecting messages
// rather than the pipeline doing work.
//
// So the venue column is now honoured, and a capture that yields no
// deltas at all is an error. A benchmark whose only failure mode is a
// confident wrong number is worse than one that does not run.
int run_ingest_bench_cmd(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string path(args[0]);
  basis::feed::FeedLogReader reader(path);
  if (!reader.ok()) {
    basis::log::error("ingest-bench: cannot open " + path);
    return 1;
  }
  // Load the whole capture first: this measures parsing, not file IO.
  std::vector<basis::feed::FeedLogRecord> records;
  while (auto record = reader.next()) {
    records.push_back(std::move(*record));
  }
  if (records.empty()) {
    basis::log::error("ingest-bench: no usable records in " + path);
    return 1;
  }

  // One parser per venue, held across the run: the parsers own simdjson
  // buffers that are worth reusing, and a capture may interleave venues.
  basis::feed::BinanceParser binance;
  basis::feed::CoinbaseParser coinbase;
  basis::feed::KalshiParser kalshi;
  basis::feed::PolymarketParser polymarket;
  basis::model::UnifiedBook book;

  std::array<std::uint64_t, basis::model::kVenueCount> per_venue{};
  std::uint64_t deltas = 0, ok = 0, ignored = 0, malformed = 0;
  std::uint64_t unrepresentable = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& record : records) {
    basis::feed::ParseResult r = [&] {
      switch (record.venue) {
        case basis::model::Venue::Binance:
          return binance.parse(record.payload, record.recv_ns);
        case basis::model::Venue::Coinbase:
          return coinbase.parse(record.payload, record.recv_ns);
        case basis::model::Venue::Kalshi:
          return kalshi.parse(record.payload, record.recv_ns);
        case basis::model::Venue::Polymarket:
          return polymarket.parse(record.payload, record.recv_ns);
      }
      return basis::feed::ParseResult{};
    }();
    switch (r.status) {
      case basis::feed::ParseStatus::Ok: ++ok; break;
      case basis::feed::ParseStatus::Ignored: ++ignored; break;
      case basis::feed::ParseStatus::Malformed: ++malformed; break;
    }
    unrepresentable += r.levels_unrepresentable;
    for (const auto& d : r.deltas) {
      book.apply(d);
      ++deltas;
    }
    ++per_venue[static_cast<std::size_t>(record.venue)];
  }
  const double ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0).count();

  const double venue_span_s =
      static_cast<double>(records.back().recv_ns - records.front().recv_ns) /
      1e9;
  const double engine_rate = ms > 0.0 ? records.size() * 1000.0 / ms : 0.0;
  const double venue_rate =
      venue_span_s > 0.0 ? records.size() / venue_span_s : 0.0;

  std::string venues;
  for (int v = 0; v < basis::model::kVenueCount; ++v) {
    if (per_venue[static_cast<std::size_t>(v)] == 0) continue;
    if (!venues.empty()) venues += ' ';
    venues += basis::model::to_string(static_cast<basis::model::Venue>(v));
    venues += '=';
    venues += std::to_string(per_venue[static_cast<std::size_t>(v)]);
  }
  std::printf("INGEST records=%llu ok=%llu ignored=%llu malformed=%llu "
              "deltas=%llu\n",
              u(records.size()), u(ok), u(ignored), u(malformed), u(deltas));
  std::printf("INGEST venues %s bad_lines=%llu unrepresentable_levels=%llu\n",
              venues.c_str(), u(reader.malformed_lines()), u(unrepresentable));
  std::printf("INGEST venue_span_s=%.1f venue_msgs_per_sec=%.0f\n",
              venue_span_s, venue_rate);
  std::printf("INGEST engine_ms=%.1f engine_msgs_per_sec=%.0f "
              "engine_deltas_per_sec=%.0f headroom=%.0fx\n",
              ms, engine_rate,
              ms > 0.0 ? deltas * 1000.0 / ms : 0.0,
              venue_rate > 0.0 ? engine_rate / venue_rate : 0.0);

  // The guard the old version lacked. Zero deltas means the timed loop
  // did no pipeline work, so every rate above describes rejection cost.
  if (deltas == 0) {
    basis::log::error("ingest-bench: no deltas parsed from " + path +
                      "; the rates above measure rejection, not ingest");
    return 1;
  }
  return 0;
}

}  // namespace basis::cli
