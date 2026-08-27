#include "bench/stats_report.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "analytics/consensus.h"
#include "core/logger.h"

namespace basis::bench {
namespace {

// printf's %llu wants unsigned long long; our counters are uint64_t. One
// short name instead of a 40-character cast at every use site.
unsigned long long u(std::uint64_t v) {
  return static_cast<unsigned long long>(v);
}

}  // namespace

// The --json output: one machine-readable object, nothing else.
//
// stats_report.cpp was 625 lines holding two outputs with different
// audiences and different reasons to change: one is parsed by scripts and
// by CI, the other is read by a person. Keeping them in one file meant a
// change to the console layout sat in the same diff as a change to a
// machine-readable contract.

void print_stats_json(const ReplayStats& stats,
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
  if (stats.replay_speed > 0.0) {
    std::printf("  \"response_us\": {\"p50\": %.3f, \"p90\": %.3f, "
                "\"p99\": %.3f, \"max\": %.3f},\n",
                static_cast<double>(stats.response_latency.p50_ns) / 1e3,
                static_cast<double>(stats.response_latency.p90_ns) / 1e3,
                static_cast<double>(stats.response_latency.p99_ns) / 1e3,
                static_cast<double>(stats.response_latency.max_ns) / 1e3);
    std::printf("  \"pacing\": {\"speed\": %.1f, \"records_late\": %llu, "
                "\"max_lag_us\": %.3f, \"pacer_overshoots\": %llu, "
                "\"max_overshoot_us\": %.3f},\n",
                stats.replay_speed, u(stats.records_late),
                static_cast<double>(stats.max_lag_ns) / 1e3,
                u(stats.pacer_overshoots),
                static_cast<double>(stats.max_overshoot_ns) / 1e3);
  }
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
                                const ReplayStats::
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
                "\"kalshi_imbalance_mean\": %.4f, "
                "\"poly_imbalance_mean\": %.4f, "
                "\"kalshi_micro_minus_mid_mean\": %.4f, "
                "\"poly_micro_minus_mid_mean\": %.4f, "
                "\"micro_basis_mean\": %.4f, "
                "\"micro_basis_samples\": %llu, "
                "\"internal_cross_updates\": %llu, "
                "\"internal_two_sided_updates\": %llu, "
                "\"internal_cross_mean_cents\": %.4f, "
                "\"internal_cross_max_cents\": %.4f, "
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
                e.kalshi_imbalance_mean, e.poly_imbalance_mean,
                e.kalshi_micro_minus_mid_mean, e.poly_micro_minus_mid_mean,
                e.micro_basis_mean, u(e.micro_basis_samples),
                u(e.internal_cross_updates), u(e.internal_two_sided_updates),
                e.internal_cross_mean_cents, e.internal_cross_max_cents,
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

// One event's full report: the basis distribution, the crossed
// episodes and their economics, quote staleness, and the microprice
// and queue-position detail. Pulled out of print_stats because it was
// two thirds of a three-hundred-line function, and it is the part a
// reader is usually looking for.

}  // namespace basis::bench
