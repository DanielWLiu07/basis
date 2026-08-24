#include "bench/stats_report.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

bool write_episodes_csv(const std::string& path,
                        const ReplayStats& stats) {
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
        for (int t = 0; t < ReplayStats::kReactionTaus; ++t) {
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

// Machine-readable replay results, for the CI perf gate and any other
// consumer that should not scrape human text. Event ids come from the
// registry and are kebab-case, so they need no JSON escaping. Allocation
// fields are emitted only in --alloc count mode (negative when absent).
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
void print_event(const ReplayStats::EventReport& event) {
  std::printf("\nevent %s\n", event.event_id.c_str());
  if (event.basis_samples == 0) {
    std::printf("  basis    no overlap (one venue never had a "
                "two-sided book)\n");
    return;
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
      // Complementary YES/NO no-arbitrage: riskless if it ever holds,
      // and the size of it decides whether it is tradeable or noise.
      if (event.internal_cross_updates > 0) {
        std::printf("           complementary arb: %llu/%llu updates had "
                    "YES_bid + NO_bid > $1.00, by %.1fc mean / %.0fc max\n",
                    u(event.internal_cross_updates),
                    u(event.internal_two_sided_updates),
                    event.internal_cross_mean_cents,
                    event.internal_cross_max_cents);
      }
      // Size-weighted view: where the microprice sits relative to the
      // mid the basis is built on, and how lopsided each queue is.
      if (event.micro_basis_samples > 0) {
        std::printf("           microprice: kalshi imbalance %+.2f "
                    "(micro-mid %+.2fc), poly imbalance %+.2f "
                    "(micro-mid %+.2fc), micro basis %+.2fc vs mid basis "
                    "%+.2fc\n",
                    event.kalshi_imbalance_mean,
                    event.kalshi_micro_minus_mid_mean,
                    event.poly_imbalance_mean,
                    event.poly_micro_minus_mid_mean,
                    event.micro_basis_mean, event.basis_mean);
      }
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

void print_stats(const ReplayStats& stats) {
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
  // Service time above, response time here. They separate only once the
  // engine stops keeping up with the schedule, which is the whole reason
  // to measure both: an unpaced replay reports the first one and calls it
  // latency, and that number stays flattering exactly when it should not.
  if (stats.replay_speed > 0.0) {
    const auto& rsp = stats.response_latency;
    std::printf("latency   response (from intended arrival) at %.0fx real "
                "rate (us): p50 %.1f  p90 %.1f  p99 %.1f  max %.1f\n",
                stats.replay_speed,
                static_cast<double>(rsp.p50_ns) / 1e3,
                static_cast<double>(rsp.p90_ns) / 1e3,
                static_cast<double>(rsp.p99_ns) / 1e3,
                static_cast<double>(rsp.max_ns) / 1e3);
    const auto pct = [&](std::uint64_t n) {
      return stats.records == 0 ? 0.0
          : 100.0 * static_cast<double>(n) /
            static_cast<double>(stats.records);
    };
    std::printf("latency   engine behind on %llu of %llu records (%.2f%%), "
                "worst backlog %.1f us\n",
                u(stats.records_late), u(stats.records),
                pct(stats.records_late),
                static_cast<double>(stats.max_lag_ns) / 1e3);
    // The instrument's own error, reported next to the engine's so the
    // reader can tell how much of the tail is the harness oversleeping.
    std::printf("latency   pacer overshoot on %llu records (%.2f%%), "
                "worst %.1f us\n",
                u(stats.pacer_overshoots), pct(stats.pacer_overshoots),
                static_cast<double>(stats.max_overshoot_ns) / 1e3);
  }

  // Mutually exclusive baskets: the mid-price sum is the venue's total
  // probability mass on the listed outcomes; the best-bid sum is checked
  // against the hard $1 no-arbitrage bound (bid-sum above $1 means selling
  // every outcome locks in riskless profit, exhaustive basket or not).
  for (const auto& b : stats.baskets) {
    const auto print_venue = [&](const char* venue,
                                 const ReplayStats::
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
    print_event(event);
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

}  // namespace basis::bench
