#include "bench/replay_harness.h"

#include "model/fees.h"

#include <algorithm>
#include <optional>

#include "core/time.h"
#include "feed/feed_log.h"

namespace basis::bench {

ReplayHarness::ReplayHarness(const normalize::ContractRegistry& registry,
                             api::InProcessSession* session,
                             analytics::LeadLagConfig lead_lag_config,
                             std::pmr::memory_resource* book_mr)
    : registry_(registry),
      session_(session),
      lead_lag_config_(lead_lag_config),
      normalizer_(registry_, book_mr) {
  normalizer_.set_observer([this](const std::string& event_id,
                                  const model::UnifiedBook& book,
                                  const model::BookDelta& delta) {
    on_event_update(event_id, book, delta);
  });
  for (const auto& b : registry_.baskets()) {
    BasketState state;
    state.id = b.id;
    state.members = b.members;
    for (const auto& m : b.members) {
      event_to_basket_.emplace(m, baskets_.size());
    }
    baskets_.push_back(std::move(state));
  }
}

// A delta for any basket member re-prices the whole group on that venue.
// Sample only when every member is two-sided there: a partial basket sum
// would understate the group and make both bounds meaningless.
void ReplayHarness::observe_basket(const std::string& event_id,
                                   const model::BookDelta& delta) {
  const auto bit = event_to_basket_.find(event_id);
  if (bit == event_to_basket_.end()) return;
  BasketState& basket = baskets_[bit->second];
  const int v = delta.venue == model::Venue::Kalshi ? 0 : 1;
  double mid_sum = 0.0;
  double bid_sum = 0.0;
  std::uint64_t two_sided = 0;
  for (const auto& member : basket.members) {
    const model::UnifiedBook* ub = normalizer_.book(member);
    if (!ub) continue;
    const auto& side = ub->book(delta.venue);
    const auto bid = side.best_bid();
    const auto mid = side.mid();
    if (!bid || !mid || !side.best_ask()) continue;
    ++two_sided;
    mid_sum += *mid / 100.0;
    bid_sum += static_cast<double>(*bid) / 100.0;
  }
  basket.max_two_sided[v] = std::max(basket.max_two_sided[v], two_sided);
  if (two_sided != basket.members.size()) return;
  basket.mid_sum[v].observe(mid_sum);
  basket.bid_sum_max[v] = std::max(basket.bid_sum_max[v], bid_sum);
  ++basket.samples[v];
}

void ReplayHarness::on_event_update(const std::string& event_id,
                                    const model::UnifiedBook& book,
                                    const model::BookDelta& delta) {
  const auto kalshi_mid = book.mid(model::Venue::Kalshi);
  const auto poly_mid = book.mid(model::Venue::Polymarket);

  auto it = analytics_.find(event_id);
  if (it == analytics_.end()) {
    it = analytics_.emplace(event_id, EventAnalytics(lead_lag_config_)).first;
  }
  // The venue this delta came from is fresh by definition; a basis sample
  // is only as live as the other venue's last update.
  if (delta.venue == model::Venue::Kalshi) {
    it->second.last_kalshi_ns = delta.ts_ns;
  } else {
    it->second.last_poly_ns = delta.ts_ns;
  }
  if (kalshi_mid && poly_mid) {
    auto& ea = it->second;
    const std::int64_t other_last = delta.venue == model::Venue::Kalshi
                                        ? ea.last_poly_ns
                                        : ea.last_kalshi_ns;
    const std::int64_t age = delta.ts_ns - other_last;
    ea.stalest_quote_ns = std::max(ea.stalest_quote_ns, age);
    if (age > kStaleQuoteNs) ++ea.stale_basis_samples;
    ea.divergence.observe(*kalshi_mid - *poly_mid);
    ea.lead_lag.observe(*kalshi_mid, *poly_mid, delta.ts_ns);
    ea.event_study.observe(*kalshi_mid, *poly_mid, delta.ts_ns);
  }
  // Track each venue's bid-ask spread whenever it is two-sided, so the basis
  // can be read against the cost of crossing each book.
  const auto spread_of = [](const model::OrderBook& b) -> std::optional<double> {
    const auto bid = b.best_bid();
    const auto ask = b.best_ask();
    if (!bid || !ask) return std::nullopt;
    return static_cast<double>(*ask - *bid);
  };
  observe_basket(event_id, delta);

  const auto& kb = book.book(model::Venue::Kalshi);
  const auto& pb = book.book(model::Venue::Polymarket);
  if (const auto s = spread_of(kb)) it->second.kalshi_spread.observe(*s);
  if (const auto s = spread_of(pb)) it->second.poly_spread.observe(*s);

  // A crossable cross-venue dislocation: one venue's best bid sits above the
  // other's best ask, so the two markets are crossed and (fees aside) the gap
  // is capturable -- buy the cheaper ask, sell into the richer bid. This is a
  // real opportunity, distinct from a mid gap that never clears the spreads.
  // Count these against all two-sided updates.
  const auto kbid = kb.best_bid(), kask = kb.best_ask();
  const auto pbid = pb.best_bid(), pask = pb.best_ask();
  if (kbid && kask && pbid && pask) {
    auto& ea = it->second;
    ++ea.two_sided_updates;
    if (*kbid > *pask || *pbid > *kask) {
      ++ea.crossable_updates;
      // Depth: how many cents the crossed side clears, whichever way it
      // crosses (at most one direction can be positive at a time).
      const bool kalshi_rich = *kbid > *pask;
      const int depth = kalshi_rich ? *kbid - *pask : *pbid - *kask;
      ea.cross_depth.observe(static_cast<double>(depth));
      // Executable edge at the touch: sell into the richer bid, lift the
      // cheaper ask; one taker order can cross at most the smaller touch
      // size. Sizes are present whenever the sides are (zero levels are
      // removed on apply), so value_or(0) is only a type-level fallback.
      const auto& rich = kalshi_rich ? kb : pb;
      const auto& cheap = kalshi_rich ? pb : kb;
      const std::int64_t contracts =
          std::min(rich.best_bid_size().value_or(0),
                   cheap.best_ask_size().value_or(0));
      const double edge_dollars = static_cast<double>(depth) *
                                  static_cast<double>(contracts) / 100.0;
      ea.cross_edge.observe(edge_dollars);
      // Beyond the touch: everything the crossed depth held, walking both
      // books to exhaustion.
      ea.cross_sweep.observe(
          static_cast<double>(model::crossed_sweep_cents(rich, cheap)) / 100.0);
      // Net of fees: the Kalshi leg executes at Kalshi's touch (its bid
      // when Kalshi is rich, its ask when it is cheap) and pays the taker
      // fee; the Polymarket leg is fee-free. Crossable does not mean
      // profitable - this is the number that decides.
      const int kalshi_leg_price = kalshi_rich ? *kbid : *kask;
      const std::int64_t fee_cents =
          model::kalshi_taker_fee_cents(contracts, kalshi_leg_price);
      const double net_dollars =
          edge_dollars - static_cast<double>(fee_cents) / 100.0;
      ea.cross_net_edge.observe(net_dollars);
      if (net_dollars > 0.0) ++ea.profitable_updates;
      // The optimal fee-aware sweep: only the fills in the crossed depth
      // that still clear their per-fill Kalshi taker fee. What a taker
      // would actually execute, as opposed to what the gross sweep says
      // was on the screen.
      const auto sweep_fill =
          model::crossed_sweep_net(rich, cheap, kalshi_rich);
      const double net_sweep_dollars =
          static_cast<double>(sweep_fill.net_cents) / 100.0;
      ea.cross_net_sweep.observe(net_sweep_dollars);
      if (sweep_fill.contracts > 0) ++ea.sweepable_updates;
      if (!ea.in_cross) {
        ea.in_cross = true;
        ea.episodes.push_back(
            {.start_ns = delta.ts_ns, .end_ns = delta.ts_ns});
        ea.open_episode_last_sweep = 0.0;
      }
      // Extend the run through this update; the report derives episode
      // count and longest span from the records, so the per-episode
      // fields below are the single source of truth.
      auto& ep = ea.episodes.back();
      ep.end_ns = delta.ts_ns;
      ++ep.updates;
      ep.depth_max_cents = std::max(ep.depth_max_cents,
                                    static_cast<double>(depth));
      ep.edge_max_dollars = std::max(ep.edge_max_dollars, edge_dollars);
      // Reaction-latency survival: the edge standing tau after the open
      // is the value from the last update at or before that instant. The
      // opening update fills tau = 0 with its own value; a later update
      // crossing a tau boundary fills it with the value that was standing
      // through the gap. Episodes that uncross first leave alive false.
      for (int t = 0; t < ReplayStats::kReactionTaus; ++t) {
        if (ep.alive_after[t]) continue;
        if (delta.ts_ns - ep.start_ns >= ReplayStats::kReactionTauNs[t]) {
          ep.alive_after[t] = true;
          ep.net_sweep_after_dollars[t] =
              ReplayStats::kReactionTauNs[t] == 0
                  ? net_sweep_dollars
                  : ea.open_episode_last_sweep;
        }
      }
      ea.open_episode_last_sweep = net_sweep_dollars;
    } else {
      ea.in_cross = false;
    }
  }

  if (!session_) return;
  if (kalshi_mid) {
    session_->publish({.event_id = event_id, .field = "kalshi_mid",
                       .value = *kalshi_mid, .ts_ns = delta.ts_ns});
  }
  if (poly_mid) {
    session_->publish({.event_id = event_id, .field = "poly_mid",
                       .value = *poly_mid, .ts_ns = delta.ts_ns});
  }
  if (kalshi_mid && poly_mid) {
    session_->publish({.event_id = event_id, .field = "basis",
                       .value = *kalshi_mid - *poly_mid, .ts_ns = delta.ts_ns});
  }
}

std::optional<ReplayStats> ReplayHarness::run(const std::string& feedlog_path,
                                              std::string* error) {
  feed::FeedLogReader reader(feedlog_path);
  if (!reader.ok()) {
    if (error) *error = "cannot open " + feedlog_path;
    return std::nullopt;
  }

  std::pmr::memory_resource* parse_mr =
      parse_arena_ ? parse_arena_->resource()
                   : std::pmr::get_default_resource();

  while (const auto record = reader.next()) {
    ++stats_.records;

    // Track the wall-clock span the capture covers (receive timestamps),
    // which gives the venue's real ingest rate, distinct from how fast the
    // engine replays. Timestamps are non-decreasing in a real capture.
    if (stats_.records == 1) stats_.first_recv_ns = record->recv_ns;
    stats_.last_recv_ns = record->recv_ns;

    // The measured span: raw bytes in, analytics observed and updates
    // published out. This is the ingest-to-signal latency PLAN.md defines;
    // the feedlog read stays outside it, standing in for the network.
    const auto t0 = time::mono_ns();

    {
      const bool is_kalshi = record->venue == model::Venue::Kalshi;
      if (is_kalshi) {
        ++stats_.kalshi_messages;
      } else {
        ++stats_.polymarket_messages;
      }
      const feed::ParseResult parsed =
          is_kalshi ? kalshi_.parse(record->payload, record->recv_ns, parse_mr)
                    : polymarket_.parse(record->payload, record->recv_ns,
                                        parse_mr);
      const auto t_parsed = breakdown_ ? time::mono_ns() : std::int64_t{0};

      switch (parsed.status) {
        case feed::ParseStatus::Ok:
          stats_.deltas += parsed.deltas.size();
          break;
        case feed::ParseStatus::Ignored:
          ++stats_.ignored;
          break;
        case feed::ParseStatus::Malformed:
          ++stats_.malformed;
          break;
      }
      if (parsed.gap) ++stats_.gaps;
      stats_.hashes_verified += parsed.hashes_verified;
      stats_.hashes_mismatched += parsed.hashes_mismatched;
      stats_.hashes_unverifiable += parsed.hashes_unverifiable;

      for (const auto& delta : parsed.deltas) {
        normalizer_.on_delta(delta);
      }

      if (breakdown_) {
        stats_.parse_ns_total += t_parsed - t0;
        stats_.downstream_ns_total += time::mono_ns() - t_parsed;
      }
    }
    // parsed is gone; an arena can now drop the whole message at once.
    if (parse_arena_) parse_arena_->release();

    const auto span = time::mono_ns() - t0;
    latency_.record(span);
    stats_.pipeline_ns += span;
  }

  stats_.malformed_lines = reader.malformed_lines();
  stats_.unmapped_deltas = normalizer_.unmapped_deltas();
  stats_.latency = latency_.report();

  stats_.events.clear();
  stats_.baskets.reserve(baskets_.size());
  for (const auto& b : baskets_) {
    ReplayStats::BasketReport report;
    report.basket_id = b.id;
    report.members = b.members.size();
    const auto venue_report = [&](int v) {
      ReplayStats::BasketVenueReport r;
      r.samples = b.samples[v];
      r.max_two_sided = b.max_two_sided[v];
      if (b.samples[v] > 0) {
        r.mid_sum_mean_dollars = b.mid_sum[v].mean();
        r.mid_sum_min_dollars = b.mid_sum[v].min();
        r.mid_sum_max_dollars = b.mid_sum[v].max();
        r.bid_sum_max_dollars = b.bid_sum_max[v];
      }
      return r;
    };
    report.kalshi = venue_report(0);
    report.polymarket = venue_report(1);
    stats_.baskets.push_back(std::move(report));
  }
  stats_.events.reserve(analytics_.size());
  for (const auto& [event_id, ea] : analytics_) {
    ReplayStats::EventReport report;
    report.event_id = event_id;
    report.basis_samples = ea.divergence.samples();
    if (report.basis_samples > 0) {
      report.basis_mean = ea.divergence.mean();
      report.basis_stddev = ea.divergence.stddev();
      report.basis_zscore = ea.divergence.zscore();
      report.basis_min = ea.divergence.min();
      report.basis_max = ea.divergence.max();
      report.basis_last = ea.divergence.last();
      report.basis_ar1 = ea.divergence.ar1_coefficient();
      report.basis_halflife_updates = ea.divergence.reversion_halflife_updates();
    }
    if (ea.kalshi_spread.samples() > 0) {
      report.kalshi_spread_mean = ea.kalshi_spread.mean();
    }
    if (ea.poly_spread.samples() > 0) {
      report.poly_spread_mean = ea.poly_spread.mean();
    }
    report.two_sided_updates = ea.two_sided_updates;
    report.crossable_updates = ea.crossable_updates;
    report.crossable_episodes = ea.episodes.size();
    report.crossable_longest_ns = 0;
    for (const auto& ep : ea.episodes) {
      report.crossable_longest_ns =
          std::max(report.crossable_longest_ns, ep.end_ns - ep.start_ns);
    }
    if (ea.cross_depth.samples() > 0) {
      report.crossable_depth_mean = ea.cross_depth.mean();
      report.crossable_depth_max = ea.cross_depth.max();
      report.crossable_edge_mean_dollars = ea.cross_edge.mean();
      report.crossable_edge_max_dollars = ea.cross_edge.max();
      report.crossable_sweep_mean_dollars = ea.cross_sweep.mean();
      report.crossable_sweep_max_dollars = ea.cross_sweep.max();
      report.crossable_net_edge_mean_dollars = ea.cross_net_edge.mean();
      report.crossable_net_edge_max_dollars = ea.cross_net_edge.max();
      report.crossable_profitable_updates = ea.profitable_updates;
      report.crossable_net_sweep_mean_dollars = ea.cross_net_sweep.mean();
      report.crossable_net_sweep_max_dollars = ea.cross_net_sweep.max();
      report.crossable_sweepable_updates = ea.sweepable_updates;
      for (int t = 0; t < ReplayStats::kReactionTaus; ++t) {
        std::uint64_t alive = 0;
        double sum = 0.0;
        for (const auto& ep : ea.episodes) {
          if (ep.alive_after[t]) ++alive;
          sum += ep.net_sweep_after_dollars[t];  // expired episodes add 0
        }
        report.episodes_alive_after[t] = alive;
        report.episode_net_sweep_after_mean_dollars[t] =
            ea.episodes.empty() ? 0.0
                                : sum / static_cast<double>(ea.episodes.size());
      }
    }
    report.episodes = ea.episodes;
    report.stale_basis_samples = ea.stale_basis_samples;
    report.stalest_quote_seconds =
        static_cast<double>(ea.stalest_quote_ns) / 1e9;
    report.lead_lag = ea.lead_lag.estimate();
    report.event_study = ea.event_study.estimate();
    stats_.events.push_back(std::move(report));
  }
  std::sort(stats_.events.begin(), stats_.events.end(),
            [](const auto& a, const auto& b) { return a.event_id < b.event_id; });

  return stats_;
}

ReplaySummary summarize(const ReplayStats& stats, std::size_t top_n) {
  ReplaySummary s;
  s.events_tracked = stats.events.size();
  for (const auto& e : stats.events) {
    if (e.basis_samples > 0) ++s.events_with_overlap;
    s.basis_samples += e.basis_samples;
    s.crossable_updates += e.crossable_updates;
    s.crossable_episodes += e.crossable_episodes;
    if (e.crossable_updates == 0) continue;
    ++s.events_crossable;
    s.top_by_edge.push_back({e.event_id, e.crossable_updates,
                             e.crossable_episodes, e.crossable_longest_ns,
                             e.crossable_edge_mean_dollars,
                             e.crossable_edge_max_dollars,
                             e.episode_net_sweep_after_mean_dollars[
                                 ReplayStats::kTau100msIndex],
                             e.episodes_alive_after[
                                 ReplayStats::kTau100msIndex]});
  }
  std::sort(s.top_by_edge.begin(), s.top_by_edge.end(),
            [](const auto& a, const auto& b) {
              if (a.edge_max_dollars != b.edge_max_dollars)
                return a.edge_max_dollars > b.edge_max_dollars;
              if (a.edge_mean_dollars != b.edge_mean_dollars)
                return a.edge_mean_dollars > b.edge_mean_dollars;
              return a.event_id < b.event_id;
            });
  if (s.top_by_edge.size() > top_n) s.top_by_edge.resize(top_n);
  return s;
}

}  // namespace basis::bench
