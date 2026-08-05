#include <charconv>
#include <cstdio>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analytics/consensus.h"
#include "bench/replay_harness.h"
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
#include <csignal>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "analytics/divergence.h"
#include "analytics/lead_lag.h"
#include "core/bounded_queue.h"
#include "feed/feed_log.h"
#include "feed/kalshi_feed.h"
#include "feed/polymarket_feed.h"
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
#ifdef BASIS_HAS_NET
      "\n"
      "  basis record <out.feedlog> [--config <contracts.toml>] [--seconds N]\n"
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
bool has_flag(const std::vector<std::string_view>& args,
              std::string_view name) {
  for (const auto& arg : args) {
    if (arg == name) return true;
  }
  return false;
}

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

// Machine-readable replay results, for the CI perf gate and any other
// consumer that should not scrape human text. Event ids come from the
// registry and are kebab-case, so they need no JSON escaping. Allocation
// Writes one row per crossed run, in time order: the distribution behind
// the report's count/longest aggregates, ready for a histogram of how
// long dislocations persist and what they were worth. Sits beside the
// other two report emitters (print_stats, print_stats_json).
bool write_episodes_csv(const std::string& path,
                        const basis::bench::ReplayStats& stats) {
  {
    std::ofstream ep_csv(path, std::ios::trunc);
    if (!ep_csv) {
      basis::log::error("cannot open --episodes-csv file: " + path);
      return false;
    }
    // The four survival columns carry the reaction-latency ladder per
    // episode: the fee-aware optimal sweep standing at the open and at
    // 50/100/250 ms after it, empty when the episode expired before the
    // rung (an empty cell is "the opportunity was gone", distinct from a
    // standing value of 0.00).
    ep_csv << "event_id,start_ns,end_ns,duration_ms,updates,"
              "depth_max_cents,edge_max_dollars,"
              "sweep_open_dollars,sweep_50ms_dollars,"
              "sweep_100ms_dollars,sweep_250ms_dollars\n";
    char row[320];
    for (const auto& event : stats.events) {
      for (const auto& ep : event.episodes) {
        int n = std::snprintf(row, sizeof(row),
                      "%s,%lld,%lld,%.4f,%llu,%.2f,%.2f",
                      event.event_id.c_str(),
                      static_cast<long long>(ep.start_ns),
                      static_cast<long long>(ep.end_ns),
                      static_cast<double>(ep.end_ns - ep.start_ns) / 1e6,
                      u(ep.updates),
                      ep.depth_max_cents, ep.edge_max_dollars);
        for (int t = 0; t < basis::bench::ReplayStats::kReactionTaus; ++t) {
          if (ep.alive_after[t]) {
            n += std::snprintf(row + n, sizeof(row) - static_cast<std::size_t>(n),
                               ",%.2f", ep.net_sweep_after_dollars[t]);
          } else {
            n += std::snprintf(row + n, sizeof(row) - static_cast<std::size_t>(n),
                               ",");
          }
        }
        ep_csv << row << "\n";
      }
    }
  }
  return true;
}

// fields are emitted only in --alloc count mode (negative when absent).
void print_stats_json(const basis::bench::ReplayStats& stats,
                      double parse_per_msg, double parse_bytes_per_msg,
                      double book_per_msg) {
  const double rps = stats.pipeline_ns > 0
      ? static_cast<double>(stats.records) * 1e9 /
            static_cast<double>(stats.pipeline_ns)
      : 0.0;

  std::printf("{\n");
  std::printf("  \"records\": %llu,\n", u(stats.records));
  std::printf("  \"kalshi_messages\": %llu,\n", u(stats.kalshi_messages));
  std::printf("  \"polymarket_messages\": %llu,\n",
              u(stats.polymarket_messages));
  std::printf("  \"deltas\": %llu,\n", u(stats.deltas));
  std::printf("  \"unmapped_deltas\": %llu,\n", u(stats.unmapped_deltas));
  std::printf("  \"ignored\": %llu,\n", u(stats.ignored));
  std::printf("  \"malformed\": %llu,\n", u(stats.malformed));
  std::printf("  \"malformed_lines\": %llu,\n", u(stats.malformed_lines));
  std::printf("  \"gaps\": %llu,\n", u(stats.gaps));
  std::printf("  \"hashes\": {\"verified\": %llu, \"mismatched\": %llu, "
              "\"unverifiable\": %llu},\n",
              u(stats.hashes_verified), u(stats.hashes_mismatched),
              u(stats.hashes_unverifiable));
  std::printf("  \"latency_us\": {\"p50\": %.3f, \"p90\": %.3f, "
              "\"p99\": %.3f, \"max\": %.3f},\n",
              static_cast<double>(stats.latency.p50_ns) / 1e3,
              static_cast<double>(stats.latency.p90_ns) / 1e3,
              static_cast<double>(stats.latency.p99_ns) / 1e3,
              static_cast<double>(stats.latency.max_ns) / 1e3);
  std::printf("  \"pipeline\": {\"ms\": %.3f, \"records_per_sec\": %.1f},\n",
              static_cast<double>(stats.pipeline_ns) / 1e6, rps);
  const double span_s =
      static_cast<double>(stats.last_recv_ns - stats.first_recv_ns) / 1e9;
  std::printf("  \"session\": {\"span_seconds\": %.3f, \"ingest_per_sec\": "
              "%.1f},\n",
              span_s > 0.0 ? span_s : 0.0,
              span_s > 0.0 ? static_cast<double>(stats.records) / span_s : 0.0);
  if (parse_per_msg >= 0.0) {
    std::printf("  \"alloc\": {\"parse_per_msg\": %.4f, "
                "\"parse_bytes_per_msg\": %.1f, \"book_per_msg\": %.4f},\n",
                parse_per_msg, parse_bytes_per_msg, book_per_msg);
  }
  const auto summary = basis::bench::summarize(stats);
  std::printf("  \"baskets\": [");
  for (std::size_t i = 0; i < stats.baskets.size(); ++i) {
    const auto& b = stats.baskets[i];
    const auto venue_json = [&](const char* name,
                                const basis::bench::ReplayStats::
                                    BasketVenueReport& r) {
      std::printf("\"%s\": {\"samples\": %llu, "
                  "\"max_two_sided\": %llu, "
                  "\"mid_sum_mean_dollars\": %.4f, "
                  "\"mid_sum_min_dollars\": %.4f, "
                  "\"mid_sum_max_dollars\": %.4f, "
                  "\"bid_sum_max_dollars\": %.4f}",
                  name, u(r.samples), u(r.max_two_sided),
                  r.mid_sum_mean_dollars,
                  r.mid_sum_min_dollars, r.mid_sum_max_dollars,
                  r.bid_sum_max_dollars);
    };
    std::printf("%s{\"basket_id\": \"%s\", \"members\": %llu, ",
                i == 0 ? "" : ", ", b.basket_id.c_str(), u(b.members));
    venue_json("kalshi", b.kalshi);
    std::printf(", ");
    venue_json("polymarket", b.polymarket);
    std::printf("}");
  }
  std::printf("],\n");
  std::printf("  \"summary\": {\"events_tracked\": %llu, "
              "\"events_with_overlap\": %llu, \"events_crossable\": %llu, "
              "\"basis_samples\": %llu, \"crossable_updates\": %llu, "
              "\"crossable_episodes\": %llu, \"top_by_edge\": [",
              u(summary.events_tracked), u(summary.events_with_overlap),
              u(summary.events_crossable), u(summary.basis_samples),
              u(summary.crossable_updates), u(summary.crossable_episodes));
  for (std::size_t i = 0; i < summary.top_by_edge.size(); ++i) {
    const auto& r = summary.top_by_edge[i];
    std::printf("%s{\"event_id\": \"%s\", \"edge_max_dollars\": %.4f, "
                "\"edge_mean_dollars\": %.4f, \"crossable_updates\": %llu, "
                "\"crossable_episodes\": %llu, \"crossable_longest_ms\": %.4f, "
                "\"surviving_100ms_mean_dollars\": %.4f, "
                "\"episodes_alive_100ms\": %llu}",
                i == 0 ? "" : ", ", r.event_id.c_str(), r.edge_max_dollars,
                r.edge_mean_dollars, u(r.crossable_updates),
                u(r.crossable_episodes),
                static_cast<double>(r.crossable_longest_ns) / 1e6,
                r.surviving_100ms_mean_dollars, u(r.episodes_alive_100ms));
  }
  std::printf("]},\n");
  std::printf("  \"events\": [");
  for (std::size_t i = 0; i < stats.events.size(); ++i) {
    const auto& e = stats.events[i];
    const auto& ll = e.lead_lag;
    const auto& es = e.event_study;
    const auto consensus = basis::analytics::lead_consensus(ll, es);
    const char* consensus_leader =
        consensus.leader() == basis::analytics::Leader::A   ? "kalshi"
        : consensus.leader() == basis::analytics::Leader::B ? "polymarket"
                                                            : "none";
    std::printf("%s\n    {\"event_id\": \"%s\", \"basis_samples\": %llu, "
                "\"basis_mean\": %.4f, \"basis_stddev\": %.4f, "
                "\"basis_zscore\": %.4f, "
                "\"basis_last\": %.4f, "
                "\"basis_ar1\": %.4f, "
                "\"basis_halflife_updates\": %.4f, "
                "\"kalshi_spread_mean\": %.4f, "
                "\"poly_spread_mean\": %.4f, "
                "\"two_sided_updates\": %llu, "
                "\"crossable_updates\": %llu, "
                "\"crossable_episodes\": %llu, "
                "\"crossable_longest_ms\": %.4f, "
                "\"crossable_depth_mean\": %.4f, "
                "\"crossable_depth_max\": %.4f, "
                "\"crossable_edge_mean_dollars\": %.4f, "
                "\"crossable_edge_max_dollars\": %.4f, "
                "\"crossable_sweep_mean_dollars\": %.4f, "
                "\"crossable_sweep_max_dollars\": %.4f, "
                "\"crossable_net_edge_mean_dollars\": %.4f, "
                "\"crossable_net_edge_max_dollars\": %.4f, "
                "\"crossable_profitable_updates\": %llu, "
                "\"crossable_net_sweep_mean_dollars\": %.4f, "
                "\"crossable_net_sweep_max_dollars\": %.4f, "
                "\"crossable_sweepable_updates\": %llu, "
                "\"survival_open_mean_dollars\": %.4f, "
                "\"survival_50ms_mean_dollars\": %.4f, "
                "\"survival_50ms_episodes\": %llu, "
                "\"survival_100ms_mean_dollars\": %.4f, "
                "\"survival_100ms_episodes\": %llu, "
                "\"survival_250ms_mean_dollars\": %.4f, "
                "\"survival_250ms_episodes\": %llu, "
                "\"stale_basis_samples\": %llu, "
                "\"stalest_quote_seconds\": %.4f, "
                "\"lead_lag\": {\"lead_seconds\": %.4f, \"correlation\": %.4f, "
                "\"samples\": %llu, \"ci_low_seconds\": %.4f, "
                "\"ci_high_seconds\": %.4f, \"resamples\": %llu, "
                "\"significant\": %s}, "
                "\"event_study\": {\"moves\": %llu, \"followed\": %llu, "
                "\"median_follow_seconds\": %.4f, \"follow_rate_z\": %.4f, "
                "\"lead_confirmed\": %s}, "
                "\"consensus_leader\": \"%s\", \"methods_agree\": %s}",
                i == 0 ? "" : ",", e.event_id.c_str(), u(e.basis_samples),
                e.basis_mean, e.basis_stddev, e.basis_zscore, e.basis_last,
                e.basis_ar1, e.basis_halflife_updates,
                e.kalshi_spread_mean, e.poly_spread_mean,
                u(e.two_sided_updates), u(e.crossable_updates),
                u(e.crossable_episodes),
                static_cast<double>(e.crossable_longest_ns) / 1e6,
                e.crossable_depth_mean, e.crossable_depth_max,
                e.crossable_edge_mean_dollars, e.crossable_edge_max_dollars,
                e.crossable_sweep_mean_dollars, e.crossable_sweep_max_dollars,
                e.crossable_net_edge_mean_dollars,
                e.crossable_net_edge_max_dollars,
                u(e.crossable_profitable_updates),
                e.crossable_net_sweep_mean_dollars,
                e.crossable_net_sweep_max_dollars,
                u(e.crossable_sweepable_updates),
                e.episode_net_sweep_after_mean_dollars[0],
                e.episode_net_sweep_after_mean_dollars[1],
                u(e.episodes_alive_after[1]),
                e.episode_net_sweep_after_mean_dollars[2],
                u(e.episodes_alive_after[2]),
                e.episode_net_sweep_after_mean_dollars[3],
                u(e.episodes_alive_after[3]),
                u(e.stale_basis_samples), e.stalest_quote_seconds,
                ll.lead_seconds, ll.correlation, u(ll.samples),
                ll.ci_low_seconds, ll.ci_high_seconds, u(ll.resamples),
                ll.lead_is_significant() ? "true" : "false",
                u(es.moves), u(es.followed), es.median_follow_seconds,
                es.follow_rate_z(), es.lead_confirmed() ? "true" : "false",
                consensus_leader, consensus.agree() ? "true" : "false");
  }
  std::printf("%s]\n}\n", stats.events.empty() ? "" : "\n  ");
}

void print_stats(const basis::bench::ReplayStats& stats) {
  std::printf("records   %llu (kalshi %llu, polymarket %llu)\n",
              u(stats.records),
              u(stats.kalshi_messages),
              u(stats.polymarket_messages));
  const double span_s =
      static_cast<double>(stats.last_recv_ns - stats.first_recv_ns) / 1e9;
  if (span_s > 0.0) {
    std::printf("session   %.1f s span, %.0f msgs/sec ingest\n", span_s,
                static_cast<double>(stats.records) / span_s);
  }
  std::printf("deltas    %llu applied, %llu unmapped\n",
              u(stats.deltas),
              u(stats.unmapped_deltas));
  std::printf("dropped   %llu malformed msgs, %llu bad lines, "
              "%llu gaps, %llu ignored\n",
              u(stats.malformed),
              u(stats.malformed_lines),
              u(stats.gaps),
              u(stats.ignored));
  if (stats.hashes_verified + stats.hashes_mismatched +
          stats.hashes_unverifiable >
      0) {
    std::printf("integrity %llu snapshot hashes verified, %llu mismatched, "
                "%llu unverifiable (refresh form)\n",
                u(stats.hashes_verified),
                u(stats.hashes_mismatched),
                u(stats.hashes_unverifiable));
  }

  const auto& lat = stats.latency;
  std::printf("latency   ingest-to-signal per record (us): "
              "p50 %.1f  p90 %.1f  p99 %.1f  max %.1f\n",
              static_cast<double>(lat.p50_ns) / 1e3,
              static_cast<double>(lat.p90_ns) / 1e3,
              static_cast<double>(lat.p99_ns) / 1e3,
              static_cast<double>(lat.max_ns) / 1e3);

  // Mutually exclusive baskets: the mid-price sum is the venue's total
  // probability mass on the listed outcomes; the best-bid sum is checked
  // against the hard $1 no-arbitrage bound (bid-sum above $1 means selling
  // every outcome locks in riskless profit, exhaustive basket or not).
  for (const auto& b : stats.baskets) {
    const auto print_venue = [&](const char* venue,
                                 const basis::bench::ReplayStats::
                                     BasketVenueReport& r) {
      if (r.samples == 0) {
        if (r.max_two_sided > 0) {
          std::printf("basket    %s (%llu outcomes, %s): never fully "
                      "quoted; at most %llu outcomes two-sided at once\n",
                      b.basket_id.c_str(), u(b.members), venue,
                      u(r.max_two_sided));
        }
        return;
      }
      std::printf("basket    %s (%llu outcomes, %s): mid-sum mean $%.3f "
                  "[$%.3f..$%.3f], bid-sum max $%.3f "
                  "($%.3f below the $1 arb bound) over %llu samples\n",
                  b.basket_id.c_str(), u(b.members), venue,
                  r.mid_sum_mean_dollars, r.mid_sum_min_dollars,
                  r.mid_sum_max_dollars, r.bid_sum_max_dollars,
                  1.0 - r.bid_sum_max_dollars, u(r.samples));
    };
    print_venue("kalshi", b.kalshi);
    print_venue("polymarket", b.polymarket);
  }

  for (const auto& event : stats.events) {
    std::printf("\nevent %s\n", event.event_id.c_str());
    if (event.basis_samples == 0) {
      std::printf("  basis    no overlap (one venue never had a "
                  "two-sided book)\n");
      continue;
    }
    std::printf("  basis    mean %+.2fc  sd %.2fc  min %+.2fc  max %+.2fc  "
                "last %+.2fc  (%llu samples)\n",
                event.basis_mean, event.basis_stddev, event.basis_min,
                event.basis_max, event.basis_last,
                u(event.basis_samples));
    if (event.basis_stddev > 0.0) {
      // How far the latest basis sits from the session mean, in standard
      // deviations: a quick read on whether the cross-venue gap is
      // currently at a typical or an unusual level.
      std::printf("           last is %+.1f sd from the session mean\n",
                  event.basis_zscore);
    }
    // AR(1) mean reversion: how sticky the basis is and, when it reverts,
    // how many updates it takes to close half the gap back to the mean.
    if (event.basis_halflife_updates > 0.0) {
      std::printf("           mean-reverting (ar1 %.3f), half-life %.1f "
                  "updates\n",
                  event.basis_ar1, event.basis_halflife_updates);
    } else if (event.basis_samples >= 3) {
      std::printf("           not mean-reverting (ar1 %.3f)\n",
                  event.basis_ar1);
    }
    // Freshness: how much of the basis series was priced against a quote
    // the other venue had not refreshed in over 5 seconds. High numbers
    // mean the "gap" is mostly one side not quoting, not a live signal.
    if (event.stale_basis_samples > 0) {
      const double pct = 100.0 * static_cast<double>(event.stale_basis_samples) /
                         static_cast<double>(event.basis_samples);
      std::printf("           %llu samples (%.1f%%) priced on a >5s-old quote, "
                  "stalest %.1fs\n",
                  u(event.stale_basis_samples),
                  pct, event.stalest_quote_seconds);
    }
    // Bid-ask spread per venue, and whether the basis actually clears it: a
    // mid gap smaller than the cost of crossing both books is quoting noise,
    // not a tradeable dislocation.
    if (event.kalshi_spread_mean >= 0.0 && event.poly_spread_mean >= 0.0) {
      const double avg_spread =
          (event.kalshi_spread_mean + event.poly_spread_mean) / 2.0;
      std::printf("  spread   kalshi %.2fc  polymarket %.2fc  -- %s\n",
                  event.kalshi_spread_mean, event.poly_spread_mean,
                  std::abs(event.basis_mean) > avg_spread
                      ? "basis clears the spread (dislocation)"
                      : "basis within the spread (noise)");
    }
    // Crossable dislocations: updates where the books were actually crossed
    // across venues (best bid > best ask), an arbitrage the mid-based basis
    // does not by itself reveal.
    if (event.two_sided_updates > 0) {
      const double pct = 100.0 * static_cast<double>(event.crossable_updates) /
                         static_cast<double>(event.two_sided_updates);
      std::printf("  arb      %llu/%llu two-sided updates crossable (%.2f%%)\n",
                  u(event.crossable_updates),
                  u(event.two_sided_updates), pct);
      // Persistence: how long the books stay crossed once they cross. The
      // longest run is the widest window an execution engine had to act.
      if (event.crossable_episodes > 0) {
        std::printf("           %llu crossed episode%s, longest held %.1f ms, "
                    "depth mean %.1fc max %.0fc\n",
                    u(event.crossable_episodes),
                    event.crossable_episodes == 1 ? "" : "s",
                    static_cast<double>(event.crossable_longest_ns) / 1e6,
                    event.crossable_depth_mean, event.crossable_depth_max);
        // Depth times the smaller touch size: what one taker order could
        // actually capture, not just how far the mids disagreed.
        std::printf("           edge at the touch mean $%.2f  max $%.2f "
                    "per crossed update\n",
                    event.crossable_edge_mean_dollars,
                    event.crossable_edge_max_dollars);
        // The touch is one taker order at the best levels; the sweep walks
        // the whole crossed depth. Equal numbers mean the cross never ran
        // deeper than the top of book.
        std::printf("           full-depth sweep mean $%.2f  max $%.2f "
                    "per crossed update\n",
                    event.crossable_sweep_mean_dollars,
                    event.crossable_sweep_max_dollars);
        // Fees decide whether any of it was real: the Kalshi leg pays the
        // taker fee (general schedule), the Polymarket leg is free.
        std::printf("           net of Kalshi taker fees mean $%+.2f  "
                    "max $%+.2f -- %llu/%llu crossed updates profitable\n",
                    event.crossable_net_edge_mean_dollars,
                    event.crossable_net_edge_max_dollars,
                    u(
                        event.crossable_profitable_updates),
                    u(event.crossable_updates));
        // The executable answer: of the whole crossed depth, only the
        // fills that clear their own fee. What the gross sweep promised
        // versus what a fee-aware taker keeps.
        std::printf("           fee-aware optimal sweep mean $%.2f  "
                    "max $%.2f -- %llu/%llu crossed updates worth taking\n",
                    event.crossable_net_sweep_mean_dollars,
                    event.crossable_net_sweep_max_dollars,
                    u(
                        event.crossable_sweepable_updates),
                    u(event.crossable_updates));
        // The time dimension of the same answer: what is still standing
        // for a taker who needs 50/100/250 ms to react after an episode
        // opens. Means are over all episodes, expired ones counting zero,
        // so each figure is the expected edge at that reaction delay.
        std::printf("           surviving a reaction delay: open $%.2f, "
                    "50ms $%.2f (%llu/%llu eps), 100ms $%.2f (%llu/%llu), "
                    "250ms $%.2f (%llu/%llu)\n",
                    event.episode_net_sweep_after_mean_dollars[0],
                    event.episode_net_sweep_after_mean_dollars[1],
                    u(event.episodes_alive_after[1]),
                    u(event.crossable_episodes),
                    event.episode_net_sweep_after_mean_dollars[2],
                    u(event.episodes_alive_after[2]),
                    u(event.crossable_episodes),
                    event.episode_net_sweep_after_mean_dollars[3],
                    u(event.episodes_alive_after[3]),
                    u(event.crossable_episodes));
      }
    }
    const auto& ll = event.lead_lag;
    if (ll.correlation <= 0.0) {
      std::printf("  lead-lag no signal (flat or too little overlap)\n");
    } else {
      const char* leader = ll.lead_seconds >= 0 ? "kalshi" : "polymarket";
      std::printf("  lead-lag %s leads by %.3fs  "
                  "(corr %.2f over %llu samples)\n",
                  leader,
                  ll.lead_seconds >= 0 ? ll.lead_seconds : -ll.lead_seconds,
                  ll.correlation,
                  u(ll.samples));
      if (ll.resamples > 0) {
        std::printf("           95%% ci %.3fs..%.3fs "
                    "(%llu block-bootstrap resamples) -- %s\n",
                    ll.ci_low_seconds, ll.ci_high_seconds,
                    u(ll.resamples),
                    ll.lead_is_significant()
                        ? "significant (interval excludes zero)"
                        : "not significant (interval spans zero)");
      }
    }
    const auto& es = event.event_study;
    if (es.moves > 0 || es.reverse_moves > 0) {
      std::printf("  events   kalshi moves: %llu/%llu followed by "
                  "polymarket, median %.3fs  |  reverse: %llu/%llu, "
                  "median %.3fs\n",
                  u(es.followed),
                  u(es.moves),
                  es.median_follow_seconds,
                  u(es.reverse_followed),
                  u(es.reverse_moves),
                  es.reverse_median_follow_seconds);
      // Turn the four counts into a verdict: does one venue's moves get
      // answered significantly more than the other's?
      if (es.moves > 0 && es.reverse_moves > 0) {
        std::printf("           follow rate %.0f%% vs %.0f%% reverse "
                    "(z %+.1f) -- %s\n",
                    es.forward_follow_rate() * 100.0,
                    es.reverse_follow_rate() * 100.0, es.follow_rate_z(),
                    es.lead_confirmed()
                        ? "confirms a lead"
                        : "no confirmed lead");
      }
    }
    // The payoff of the two-method design: do cross-correlation and the
    // event study agree on which venue leads? Agreement is the defensible
    // finding; a conflict means the apparent lead is method-dependent.
    const auto consensus =
        basis::analytics::lead_consensus(ll, es);
    const auto leader_name = [](basis::analytics::Leader l) {
      switch (l) {
        case basis::analytics::Leader::A: return "kalshi";
        case basis::analytics::Leader::B: return "polymarket";
        default: return "neither";
      }
    };
    if (consensus.agree()) {
      std::printf("  consensus both methods agree: %s leads\n",
                  leader_name(consensus.leader()));
    } else if (consensus.conflict()) {
      std::printf("  consensus methods disagree (cross-corr %s, event study "
                  "%s) -- no reliable lead\n",
                  leader_name(consensus.crosscorr),
                  leader_name(consensus.event_study));
    }
  }

  // Session rollup: with one synthetic event this is a sanity echo; on a
  // real multi-market capture it is the headline - where the edge was.
  if (!stats.events.empty()) {
    const auto s = basis::bench::summarize(stats);
    std::printf("\nsummary  %llu events tracked, %llu with overlap, "
                "%llu crossable\n",
                u(s.events_tracked),
                u(s.events_with_overlap),
                u(s.events_crossable));
    std::printf("         %llu basis samples, %llu crossable updates in "
                "%llu episodes\n",
                u(s.basis_samples),
                u(s.crossable_updates),
                u(s.crossable_episodes));
    for (std::size_t i = 0; i < s.top_by_edge.size(); ++i) {
      const auto& r = s.top_by_edge[i];
      std::printf("         %s %s  edge max $%.2f mean $%.2f  "
                  "(%llu crossed updates, %llu episodes, longest %.1f ms, "
                  "$%.2f survives 100ms in %llu)\n",
                  i == 0 ? "best edge" : "         ", r.event_id.c_str(),
                  r.edge_max_dollars, r.edge_mean_dollars,
                  u(r.crossable_updates),
                  u(r.crossable_episodes),
                  static_cast<double>(r.crossable_longest_ns) / 1e6,
                  r.surviving_100ms_mean_dollars,
                  u(r.episodes_alive_100ms));
    }
  }
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
      !write_episodes_csv(episodes_csv_path, *stats)) {
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
    print_stats_json(*stats, parse_per_msg, parse_bytes_per_msg, book_per_msg);
    return 0;
  }

  std::printf("replayed %s against %s (alloc %s)\n\n", in_path.c_str(),
              config_path.c_str(), alloc_mode.c_str());
  print_stats(*stats);

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
  if (tokens.empty()) {
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

  basis::feed::PolymarketFeed poly_feed({.token_ids = tokens});
  poly_feed.set_raw_tap(make_tap(basis::model::Venue::Polymarket));

  // Kalshi requires an authenticated session even for market data; without
  // credentials the recording is Polymarket-only, stated up front rather
  // than discovered in the replay.
  std::unique_ptr<basis::feed::KalshiFeed> kalshi_feed;
  if (!kalshi_key_id.empty()) {
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
  std::printf("recording %zu polymarket token(s)%s -> %s%s\n", tokens.size(),
              kalshi_feed
                  ? (" + " + std::to_string(tickers.size()) +
                     " kalshi ticker(s)").c_str()
                  : " (no kalshi credentials, polymarket only)",
              out_path.c_str(), *seconds > 0 ? "" : "  (ctrl-c to stop)");
  poly_feed.start();
  if (kalshi_feed) kalshi_feed->start();

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
      std::printf("  poly %llu msgs %llu deltas %llu malformed %llu recon",
                  u(poly_feed.messages()),
                  u(poly_feed.deltas()),
                  u(poly_feed.malformed()),
                  u(poly_feed.reconnects()));
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
  poly_feed.stop();
  if (kalshi_feed) kalshi_feed->stop();

  std::printf("wrote %llu records to %s (%llu rejected)\n",
              u(written.load()),
              out_path.c_str(),
              u(rejected.load()));
  std::printf("  polymarket: %llu malformed, %llu reconnects, "
              "%llu hashes verified, %llu mismatched\n",
              u(poly_feed.malformed()),
              u(poly_feed.reconnects()),
              u(poly_feed.hashes_verified()),
              u(
                  poly_feed.hashes_mismatched()));
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
  if (command == "synth") return run_synth(rest);
  if (command == "replay") return run_replay(rest);
#ifdef BASIS_HAS_NET
  if (command == "record") return run_record(rest);
  if (command == "live") return run_live(rest);
#endif
  return usage();
}
