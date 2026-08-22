#include "cli/commands.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory_resource>
#include <string>
#include <vector>

#include "analytics/event_study.h"
#include "analytics/hayashi_yoshida.h"
#include "analytics/lead_lag.h"
#include "bench/replay_harness.h"
#include "bench/stats_report.h"
#include "bench/synth_generator.h"
#include "cli/args.h"
#include "cli/usage.h"
#include "core/counting_resource.h"
#include "core/logger.h"
#include "feed/binance_parser.h"
#include "feed/book_sequencer.h"
#include "feed/coinbase_parser.h"
#include "feed/feed_log.h"
#include "model/order_book.h"
#include "model/unified_book.h"
#include "normalize/contract_registry.h"
#include "normalize/crypto_instrument.h"

#ifdef BASIS_HAS_BDE
#include "alloc/bde_arena.h"
#endif

namespace basis::cli {

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

}  // namespace basis::cli
