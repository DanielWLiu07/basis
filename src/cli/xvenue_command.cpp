#include "cli/commands.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "analytics/event_study.h"
#include "analytics/hayashi_yoshida.h"
#include "analytics/lead_lag.h"
#include "cli/args.h"
#include "cli/usage.h"
#include "core/logger.h"
#include "feed/binance_parser.h"
#include "feed/coinbase_parser.h"
#include "feed/feed_log.h"
#include "model/order_book.h"
#include "normalize/crypto_instrument.h"

#ifdef BASIS_HAS_BDE
#endif

namespace basis::cli {

// Which venue's price moves first, by three estimators.
//
// analysis_commands.cpp held all four capture-reading subcommands and was
// the largest file in the repo at 737 lines. They share includes and
// nothing else: one generates a capture, one replays it, one measures
// which venue's price moves first, one reconciles a rebuilt book against
// the venue's own snapshot. Splitting on that seam is what the earlier
// main.cpp split did, and for the same reason.

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
  // Hayashi-Yoshida scan. The step is not a resampling interval, so it can
  // sit well under the venues' update period without inventing data.
  std::int64_t hy_step_ms = 5;
  std::int64_t hy_max_lag_ms = 2000;
  // Restricts the report to one canonical instrument. A capture with more
  // than one in it produces a block per instrument; scripts that want a
  // single block ask for it by name rather than by grepping the first hit.
  const std::string instrument_filter(flag_string(args, "--instrument", ""));
  // Writes the Hayashi-Yoshida lag scan to CSV. The point estimate is the
  // argmax of this curve, and a reader should be able to see whether that
  // argmax sits on a peak or on a plateau.
  const std::string hy_curve_path(flag_string(args, "--hy-curve", ""));
  for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
    const auto v = parse_int(args[i + 1]);
    if (!v) continue;
    if (args[i] == "--grid-ms") grid_ms = *v;
    else if (args[i] == "--max-lag-bins") max_lag_bins = static_cast<int>(*v);
    else if (args[i] == "--move-cents") move_cents = *v;
    else if (args[i] == "--follow-ms") follow_ms = *v;
    else if (args[i] == "--sample-ms") sample_ms = *v;
    else if (args[i] == "--hy-step-ms") hy_step_ms = *v;
    else if (args[i] == "--hy-max-lag-ms") hy_max_lag_ms = *v;
  }
  if (grid_ms <= 0 || max_lag_bins <= 0) {
    basis::log::error("xvenue-lead: grid-ms and max-lag-bins must be positive");
    return 1;
  }
  if (hy_step_ms <= 0 || hy_max_lag_ms < hy_step_ms) {
    basis::log::error("xvenue-lead: hy-step-ms must be positive and no larger "
                      "than hy-max-lag-ms");
    return 1;
  }

  basis::feed::FeedLogReader reader(path);
  if (!reader.ok()) {
    basis::log::error("xvenue-lead: cannot open " + path);
    return 1;
  }

  basis::feed::BinanceParser binance;
  basis::feed::CoinbaseParser coinbase;

  basis::analytics::LeadLagConfig cfg;
  cfg.grid_ns = grid_ms * 1'000'000;
  cfg.max_lag_bins = max_lag_bins;
  const basis::analytics::EventStudyConfig evcfg{
      .move_cents = static_cast<double>(move_cents),
      .follow_window_ns = follow_ms * 1'000'000};
  basis::analytics::HayashiYoshidaConfig hycfg;
  hycfg.lag_step_ns = hy_step_ms * 1'000'000;
  hycfg.max_lag_steps = static_cast<int>(hy_max_lag_ms / hy_step_ms);

  // One independent measurement per instrument. A capture with BTC and ETH
  // in it is two experiments over the same window, not one: the books must
  // not mix, and neither must the estimators, but the market conditions
  // they run under are identical, which is what makes the second
  // instrument a control on the first rather than a separate study.
  //
  // Three estimators, because they fail in different ways and agreement
  // between them is the result.
  //
  // The cross-correlation estimator resamples onto a fixed grid, so it can
  // never resolve a lead shorter than one bin. The event study works on the
  // raw irregular timestamps and answers a different question -- whose
  // moves get answered, and how fast -- so it is not bounded by the grid.
  // Hayashi-Yoshida answers the first question without the grid at all,
  // summing return products over overlapping observation intervals, which
  // is what lets it put a duration on a lead the grid can only bracket.
  struct InstrumentState {
    InstrumentState(const basis::analytics::LeadLagConfig& c,
                    const basis::analytics::EventStudyConfig& e,
                    const basis::analytics::HayashiYoshidaConfig& h)
        : est(c), ev(e), hy(h) {}
    std::array<basis::model::OrderBook, basis::model::kVenueCount> books;
    std::array<std::string, basis::model::kVenueCount> markets;
    std::array<std::uint64_t, basis::model::kVenueCount> msgs{};
    basis::analytics::CrossCorrelationEstimator est;
    basis::analytics::EventStudyEstimator ev;
    basis::analytics::HayashiYoshidaEstimator hy;
    std::int64_t next_sample_ns = 0;
    std::uint64_t observations = 0;
    std::uint64_t hy_observations = 0;
    bool mixed_markets = false;
  };
  // Ordered, so the report comes out the same way on every run regardless
  // of which instrument happened to quote first.
  std::map<std::string, InstrumentState> instruments;

  std::uint64_t records = 0, malformed = 0, unpaired = 0;
  std::uint64_t unrepresentable = 0;
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
    if (r.deltas.empty()) continue;

    // Every delta in one message carries the same market, so the canonical
    // name is resolved once per message rather than once per level.
    const auto name = basis::normalize::canonical_instrument(
        rec->venue, r.deltas.front().market);
    if (!name) { ++unpaired; continue; }
    if (!instrument_filter.empty() && *name != instrument_filter) continue;

    auto [it, inserted] = instruments.try_emplace(*name, cfg, evcfg, hycfg);
    InstrumentState& st = it->second;
    ++st.msgs[vi];
    for (const auto& d : r.deltas) {
      // Two venue symbols can canonicalise to one instrument (btcusdt and
      // btcusd both mean BTC/USD). Pairing them across venues is the point;
      // merging them into one book within a venue is not, so it is caught
      // rather than silently averaged.
      if (st.markets[vi].empty()) st.markets[vi] = std::string(d.market);
      else if (st.markets[vi] != d.market) st.mixed_markets = true;
      st.books[vi].apply(d);
    }
    // Sample whenever either venue moves, but only once both are two-sided;
    // the estimator resamples onto its own grid, so an uneven arrival rate
    // between the venues is handled there rather than by dropping data.
    const auto a = st.books[static_cast<std::size_t>(basis::model::Venue::Binance)].mid();
    const auto b = st.books[static_cast<std::size_t>(basis::model::Venue::Coinbase)].mid();
    if (a && b) {
      // Hayashi-Yoshida always gets the raw update, never the sampled one.
      // --sample-ms exists to put both venues on identical windows for the
      // other two estimators; handing that to HY would erase the very
      // asynchrony it reads the lead out of.
      st.hy.observe(*a, *b, rec->recv_ns);
      ++st.hy_observations;
      if (sample_ms <= 0) {
        st.est.observe(*a, *b, rec->recv_ns);
        st.ev.observe(*a, *b, rec->recv_ns);
        ++st.observations;
      } else {
        const std::int64_t step = sample_ms * 1'000'000;
        if (st.next_sample_ns == 0) st.next_sample_ns = rec->recv_ns;
        while (rec->recv_ns >= st.next_sample_ns) {
          st.est.observe(*a, *b, st.next_sample_ns);
          st.ev.observe(*a, *b, st.next_sample_ns);
          ++st.observations;
          st.next_sample_ns += step;
        }
      }
    }
  }

  const auto bi = static_cast<std::size_t>(basis::model::Venue::Binance);
  const auto ci = static_cast<std::size_t>(basis::model::Venue::Coinbase);
  // An instrument only one venue quoted cannot be compared. Drop it with a
  // count rather than reporting a one-sided result that would read as a
  // measurement.
  std::uint64_t single_venue = 0;
  for (auto it = instruments.begin(); it != instruments.end();) {
    if (it->second.msgs[bi] == 0 || it->second.msgs[ci] == 0) {
      ++single_venue;
      it = instruments.erase(it);
    } else {
      ++it;
    }
  }
  if (instruments.empty()) {
    basis::log::error("xvenue-lead: no instrument is quoted by both venues");
    return 1;
  }
  for (const auto& [name, st] : instruments) {
    if (st.mixed_markets) {
      basis::log::error("xvenue-lead: a venue quoted more than one product "
                        "for " + name + "; the mid would mix instruments");
      return 1;
    }
  }

  const double span_s = static_cast<double>(last_ns - first_ns) / 1e9;
  std::printf("XVENUE span_s=%.1f records=%llu malformed=%llu "
              "unrepresentable_levels=%llu unpaired_msgs=%llu "
              "single_venue_instruments=%llu instruments=%zu\n",
              span_s, u(records), u(malformed), u(unrepresentable),
              u(unpaired), u(single_venue), instruments.size());
  std::printf("XVENUE grid_ms=%lld max_lag_bins=%d sample_ms=%lld "
              "event_move_cents=%lld follow_window_ms=%lld "
              "hy_step_ms=%lld hy_max_lag_ms=%lld\n",
              static_cast<long long>(grid_ms), max_lag_bins,
              static_cast<long long>(sample_ms),
              static_cast<long long>(move_cents),
              static_cast<long long>(follow_ms),
              static_cast<long long>(hy_step_ms),
              static_cast<long long>(hy_max_lag_ms));

  // Every per-instrument line carries instrument=, so a multi-instrument
  // report stays greppable one instrument at a time.
  for (const auto& [name, st] : instruments) {
    const char* n = name.c_str();
    const auto res = st.est.estimate();
    const auto evr = st.ev.estimate();
    std::printf("XVENUE instrument=%s binance_market=%s coinbase_market=%s "
                "binance_msgs=%llu coinbase_msgs=%llu observations=%llu\n",
                n, st.markets[bi].c_str(), st.markets[ci].c_str(),
                u(st.msgs[bi]), u(st.msgs[ci]), u(st.observations));
    std::printf("XVENUE instrument=%s lead_ms=%.1f corr=%.4f samples=%llu "
                "ci_low_ms=%.1f ci_high_ms=%.1f resamples=%llu significant=%d\n",
                n, res.lead_seconds * 1000.0, res.correlation, u(res.samples),
                res.ci_low_seconds * 1000.0, res.ci_high_seconds * 1000.0,
                u(res.resamples), res.lead_is_significant() ? 1 : 0);
    const auto hyrep = st.hy.analyze();
    const auto& hyr = hyrep.lead;
    std::printf("XVENUE instrument=%s hy_lead_ms=%.1f hy_corr=%.4f "
                "hy_updates=%llu hy_ci_low_ms=%.1f hy_ci_high_ms=%.1f "
                "hy_resamples=%llu hy_significant=%d\n",
                n, hyr.lead_seconds * 1000.0, hyr.correlation,
                u(st.hy_observations), hyr.ci_low_seconds * 1000.0,
                hyr.ci_high_seconds * 1000.0, u(hyr.resamples),
                hyr.lead_is_significant() ? 1 : 0);
    std::printf("XVENUE instrument=%s hy_ratio=%.3f hy_ratio_ci_low=%.3f "
                "hy_ratio_ci_high=%.3f hy_ratio_resolved=%d\n",
                n, hyrep.ratio, hyrep.ratio_ci_low, hyrep.ratio_ci_high,
                hyrep.ratio_resolved() ? 1 : 0);
    std::printf("XVENUE instrument=%s binance_moves=%llu answered=%llu "
                "rate=%.3f median_follow_ms=%.1f\n",
                n, u(evr.moves), u(evr.followed), evr.forward_follow_rate(),
                evr.median_follow_seconds * 1000.0);
    std::printf("XVENUE instrument=%s coinbase_moves=%llu answered=%llu "
                "rate=%.3f median_follow_ms=%.1f\n",
                n, u(evr.reverse_moves), u(evr.reverse_followed),
                evr.reverse_follow_rate(),
                evr.reverse_median_follow_seconds * 1000.0);
    std::printf("XVENUE instrument=%s follow_rate_z=%.2f confirmed_leader=%d\n",
                n, evr.follow_rate_z(), evr.confirmed_leader());
  }
  if (!hy_curve_path.empty()) {
    std::ofstream out(hy_curve_path);
    if (!out) {
      basis::log::error("xvenue-lead: cannot write " + hy_curve_path);
      return 1;
    }
    out << "instrument,lag_ms,hy_correlation\n";
    for (const auto& [name, st] : instruments) {
      for (const auto& pt : st.hy.curve()) {
        out << name << ','
            << static_cast<double>(pt.lag_ns) / 1e6 << ','
            << pt.correlation << '\n';
      }
    }
    if (!out) {
      basis::log::error("xvenue-lead: write failed for " + hy_curve_path);
      return 1;
    }
  }
  std::printf("XVENUE note=positive_lead_means_binance_leads_coinbase\n");
  return 0;
}

// Reconstructs a book from a venue diff stream through the sequencer and
// checks it against a snapshot the venue produced independently. The
// capture interleaves three record kinds: diff, snapshot (the one joined
// from), and validation_snapshot (the one checked against, taken later so
// the stream runs past it).
}  // namespace basis::cli
