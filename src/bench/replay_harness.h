#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "analytics/divergence.h"
#include "analytics/event_study.h"
#include "analytics/lead_lag.h"
#include "api/in_process_session.h"
#include "bench/latency_recorder.h"
#include "feed/kalshi_parser.h"
#include "feed/polymarket_parser.h"
#include "normalize/normalizer.h"

namespace basis::bench {

// A basis sample priced while the other venue's book is older than this is
// counted as stale: the "gap" it shows may just be one side not quoting.
inline constexpr std::int64_t kStaleQuoteNs = 5'000'000'000;  // 5 seconds

// Everything one replay produced. Counters follow the no-silent-drop rule:
// every record is accounted for as deltas, ignored, or malformed.
struct ReplayStats {
  std::uint64_t records = 0;
  std::uint64_t kalshi_messages = 0;
  std::uint64_t polymarket_messages = 0;
  std::uint64_t deltas = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t malformed_lines = 0;  // broken feedlog framing
  std::uint64_t gaps = 0;
  std::uint64_t unmapped_deltas = 0;

  // Venue integrity hashes (Polymarket book snapshots): verified against
  // a locally recomputed SHA-1, mismatched, or carrying too few fields to
  // recompute (the venue's periodic refresh form).
  std::uint64_t hashes_verified = 0;
  std::uint64_t hashes_mismatched = 0;
  std::uint64_t hashes_unverifiable = 0;

  LatencyRecorder::Report latency;  // per-record ingest-to-signal

  // Response time, which is a different and less flattering quantity.
  //
  // `latency` above is SERVICE time: the clock starts when the engine
  // picks a record up. Replayed flat out that is all it can ever be,
  // because the loop never moves on until the previous record is done, so
  // a slow record delays its successors and nobody measures the delay.
  // That is coordinated omission, and it makes a p99 look good in exactly
  // the situation a real consumer would be suffering.
  //
  // Under pacing the harness knows when each record was SUPPOSED to
  // arrive, from the capture's own receive timestamps, and measures from
  // there instead. Queueing shows up where it belongs. Only populated
  // when replay_speed > 0; identical to `latency` when the engine is
  // never behind, which is the result worth having at the venue's real
  // rate.
  LatencyRecorder::Report response_latency;

  // Pacing state. speed 0 means the replay ran flat out and
  // response_latency is empty.
  double        replay_speed = 0.0;

  // Two different reasons a record can be reached after it was due, and
  // conflating them would make the harness lie about the engine.
  //
  // `records_late` counts genuine backlog: the previous record was still
  // being processed when this one came due, so the engine is behind and
  // the delay is its own. That is the quantity response time exists to
  // expose.
  //
  // `pacer_overshoots` counts the other case: the engine was idle and
  // waiting, and the sleep the harness itself was in ran long. That is
  // the instrument's error, not the engine's, and it is reported
  // separately so a reader can see how much of the tail belongs to the
  // measurement. Sleeping is only used for waits well clear of the OS
  // timer's granularity and the last stretch is spun, so it should stay
  // small; when it does not, the response-time tail is not trustworthy.
  std::uint64_t records_late = 0;
  std::int64_t  max_lag_ns = 0;      // worst backlog, engine's own
  std::uint64_t pacer_overshoots = 0;
  std::int64_t  max_overshoot_ns = 0;
  std::int64_t pipeline_ns = 0;     // sum of measured spans, excludes file io

  // Wall-clock span the capture covers, from receive timestamps. The
  // ingest rate (records over this span) is the venue's real message rate,
  // not the engine's replay throughput.
  std::int64_t first_recv_ns = 0;
  std::int64_t last_recv_ns = 0;

  // Per-stage totals, populated only in breakdown mode (set_breakdown).
  // The split is parse (venue JSON to canonical deltas) vs downstream
  // (normalize, book apply, analytics, publish). Zero when off.
  std::int64_t parse_ns_total = 0;
  std::int64_t downstream_ns_total = 0;

  // One crossed run, first to last crossed update. The aggregate episode
  // counters answer "how often and how long"; keeping each episode lets a
  // consumer see the distribution instead of just count/longest.
  // Reaction-latency ladder for episode survival: how much fee-aware
  // optimal sweep is still there for a taker who needs this long to react
  // after an episode opens. Index 0 is "at the open".
  static constexpr int kReactionTaus = 4;
  static constexpr std::int64_t kReactionTauNs[kReactionTaus] = {
      0, 50'000'000, 100'000'000, 250'000'000};
  // The rung the ranked summary quotes; the assert keeps a ladder reorder
  // from silently relabeling that number.
  static constexpr int kTau100msIndex = 2;
  static_assert(kReactionTauNs[kTau100msIndex] == 100'000'000);

  struct CrossedEpisode {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;  // last crossed update; single-update span is 0
    std::uint64_t updates = 0;
    double depth_max_cents = 0.0;
    double edge_max_dollars = 0.0;
    // Fee-aware optimal sweep still standing kReactionTauNs[i] after the
    // open: the value as of the last update at or before that instant
    // (books only change on updates, so the standing edge between updates
    // is the previous update's). alive[i] false means the episode
    // uncrossed before tau ever arrived - the opportunity was gone.
    double net_sweep_after_dollars[kReactionTaus] = {0.0, 0.0, 0.0, 0.0};
    bool   alive_after[kReactionTaus] = {false, false, false, false};
  };

  struct EventReport {
    std::string event_id;
    std::uint64_t basis_samples = 0;
    double basis_mean = 0.0;
    double basis_stddev = 0.0;
    double basis_zscore = 0.0;  // last basis in stddevs from the mean
    double basis_min = 0.0;
    double basis_max = 0.0;
    double basis_last = 0.0;
    double basis_ar1 = 0.0;             // AR(1) coefficient of the basis
    double basis_halflife_updates = 0.0;  // 0 when not mean-reverting
    // Mean bid-ask spread per venue (cents), over updates where that venue
    // had a two-sided book. -1 means the venue was never two-sided. The basis
    // is only a tradeable dislocation when it clears these; within them it is
    // quoting noise.
    double kalshi_spread_mean = -1.0;
    double poly_spread_mean = -1.0;
    // Microstructure at the touch, over updates where that venue was
    // two-sided. Queue imbalance in [-1, 1] (positive = heavier bid).
    // micro_minus_mid is the size-weighted price minus the naive mid, in
    // cents: where it is systematically nonzero, a mid-based basis is
    // measuring something the size-weighted price disagrees with. -2 is
    // the never-two-sided sentinel (the real range is [-1, 1] and the
    // spread cannot exceed 99 cents).
    double kalshi_imbalance_mean = -2.0;
    double poly_imbalance_mean = -2.0;
    double kalshi_micro_minus_mid_mean = -100.0;
    double poly_micro_minus_mid_mean = -100.0;
    // The basis recomputed on microprices instead of mids, over the same
    // samples. If the two means differ, the headline basis carries a
    // queue-imbalance bias.
    double micro_basis_mean = 0.0;
    std::uint64_t micro_basis_samples = 0;
    // Complementary (YES/NO) no-arbitrage, which only exists because these
    // contracts settle at $0 or $1: YES and NO on one market are exhaustive
    // and mutually exclusive, so a buyer of both pays $1 and receives
    // exactly $1. Selling one of each therefore collects YES_bid + NO_bid
    // against a certain $1 liability, and any excess over $1.00 is riskless.
    //
    // The normalizer folds NO into the YES frame (a NO bid at p is a YES
    // ask at 100 - p), so this condition is exactly "the folded book is
    // internally crossed": best bid above best ask on one venue. No basket
    // completeness caveat applies here, unlike the multi-outcome baskets:
    // two complementary contracts are the whole outcome space by
    // construction.
    std::uint64_t internal_cross_updates = 0;   // folded book crossed
    std::uint64_t internal_two_sided_updates = 0;
    double internal_cross_max_cents = 0.0;      // best bid minus best ask
    double internal_cross_mean_cents = 0.0;
    // Updates where both venues were two-sided, and how many of those were a
    // crossable cross-venue dislocation (best bid on one > best ask on the
    // other): an actual, fees-aside arbitrage between the books.
    std::uint64_t two_sided_updates = 0;
    std::uint64_t crossable_updates = 0;
    // Persistence of those dislocations: distinct crossed runs, and the
    // longest observed span the books stayed crossed (first to last crossed
    // update of a run, so a run seen at a single update spans 0). This is
    // the window an execution engine would have had to act in.
    std::uint64_t crossable_episodes = 0;
    std::int64_t crossable_longest_ns = 0;
    // Depth of those dislocations, in cents: how far the richer bid sat
    // above the cheaper ask across crossable updates. With frequency
    // (crossable_updates), persistence (episodes/longest), and now size,
    // the arb is fully characterized: how often, how long, how much.
    double crossable_depth_mean = 0.0;
    double crossable_depth_max = 0.0;
    // Executable edge at the touch, in dollars: depth times the smaller of
    // the two touch sizes (the most contracts one taker order could cross
    // for), per crossable update. Depth says the books crossed; this says
    // what crossing them was worth.
    double crossable_edge_mean_dollars = 0.0;
    double crossable_edge_max_dollars = 0.0;
    // Full-depth sweep, in dollars: walk the richer book's bids against the
    // cheaper book's asks to exhaustion (crossed_sweep_cents), per crossable
    // update. The touch edge is one taker order at the best levels; the
    // sweep is everything the crossed depth held. Sweep >= touch always.
    double crossable_sweep_mean_dollars = 0.0;
    double crossable_sweep_max_dollars = 0.0;
    // Touch edge net of Kalshi taker fees (model/fees.h; the Polymarket
    // leg is fee-free). Negative means the books crossed but crossing
    // them lost money - "crossable" is not "profitable". The count is how
    // many crossable updates stayed profitable after fees.
    double crossable_net_edge_mean_dollars = 0.0;
    double crossable_net_edge_max_dollars = 0.0;
    std::uint64_t crossable_profitable_updates = 0;
    // The optimal fee-aware sweep (crossed_sweep_net): take only the fills
    // in the crossed depth that still net positive after the Kalshi taker
    // fee, per crossable update. Never negative - a taker who would lose
    // money declines to trade - and never above the gross sweep; the gap
    // between the two is what fees eat. The count is how many crossable
    // updates had any fill worth taking at all.
    double crossable_net_sweep_mean_dollars = 0.0;
    double crossable_net_sweep_max_dollars = 0.0;
    std::uint64_t crossable_sweepable_updates = 0;
    // Reaction-latency survival, aggregated over episodes: how many
    // episodes were still crossed kReactionTauNs[i] after opening, and
    // the mean optimal sweep still standing then - averaged over ALL
    // episodes with expired ones contributing zero, so it reads as the
    // expected edge for a taker with that reaction delay.
    std::uint64_t episodes_alive_after[kReactionTaus] = {0, 0, 0, 0};
    double episode_net_sweep_after_mean_dollars[kReactionTaus] =
        {0.0, 0.0, 0.0, 0.0};
    // Freshness of the basis: a sample priced while the *other* venue had
    // not updated for more than kStaleQuoteNs is built on a stale quote
    // and says little about the live gap. Counted against basis_samples,
    // with the worst cross-venue quote age seen at any sample.
    std::uint64_t stale_basis_samples = 0;
    double stalest_quote_seconds = 0.0;
    // Every crossed run in time order. Invariants the aggregates already
    // promise: size() == crossable_episodes, updates sum to
    // crossable_updates, and the widest span equals crossable_longest_ns.
    std::vector<CrossedEpisode> episodes;
    analytics::LeadLagResult lead_lag;      // positive: Kalshi leads
    analytics::EventStudyResult event_study;  // independent cross-check
  };
  std::vector<EventReport> events;  // sorted by event id

  // Mutually exclusive outcome groups (basket = "..." in the registry),
  // watched per venue. The YES prices of mutually exclusive outcomes obey
  // a hard no-arbitrage bound: if the best BIDS ever sum above $1.00,
  // selling one contract of each outcome locks in a riskless profit (at
  // most one can pay out) - and that holds even when the basket lists
  // only some outcomes. The mid-price sum is the softer coherence read:
  // how much probability mass the venue assigns the listed outcomes.
  // Sampled only on updates where every member is two-sided on the venue.
  struct BasketVenueReport {
    std::uint64_t samples = 0;
    // Most members simultaneously two-sided on this venue: when samples
    // is zero this says how close the basket ever came to fully quoting.
    std::uint64_t max_two_sided = 0;
    double mid_sum_mean_dollars = 0.0;
    double mid_sum_min_dollars = 0.0;
    double mid_sum_max_dollars = 0.0;
    double bid_sum_max_dollars = 0.0;
  };
  struct BasketReport {
    std::string basket_id;
    std::uint64_t members = 0;
    BasketVenueReport kalshi;
    BasketVenueReport polymarket;
  };
  std::vector<BasketReport> baskets;  // registry file order
};

// Session-level rollup of one replay. Per-event blocks answer "what did
// this market do"; the summary answers "across the whole capture, where
// was the edge" - totals over every event plus the crossable events
// ranked by the largest executable edge seen at the touch.
struct ReplaySummary {
  std::uint64_t events_tracked = 0;
  std::uint64_t events_with_overlap = 0;  // basis_samples > 0
  std::uint64_t events_crossable = 0;     // crossable_updates > 0
  std::uint64_t basis_samples = 0;
  std::uint64_t crossable_updates = 0;
  std::uint64_t crossable_episodes = 0;

  struct RankedEvent {
    std::string event_id;
    std::uint64_t crossable_updates = 0;
    std::uint64_t crossable_episodes = 0;
    std::int64_t crossable_longest_ns = 0;
    double edge_mean_dollars = 0.0;
    double edge_max_dollars = 0.0;
    // The executability check on the same ranking: expected fee-aware
    // sweep 100 ms after an episode opens, and how many episodes were
    // still crossed then. Screen edge that never survives a realistic
    // reaction delay reads very differently from edge that does.
    double surviving_100ms_mean_dollars = 0.0;
    std::uint64_t episodes_alive_100ms = 0;
  };
  // Crossable events, best edge first (max edge, then mean edge, then
  // event id so equal events rank deterministically). At most top_n.
  std::vector<RankedEvent> top_by_edge;
};

// Pure rollup over an already-produced ReplayStats; no harness state.
ReplaySummary summarize(const ReplayStats& stats, std::size_t top_n = 5);

// Per-message allocation context for the parse path. resource() backs each
// message's ParseResult; release() runs after the message's deltas have
// been consumed, so an arena implementation can drop everything at once.
// The default-resource behavior is a null ParseArena*, not a subclass.
class ParseArena {
 public:
  virtual ~ParseArena() = default;
  virtual std::pmr::memory_resource* resource() = 0;
  virtual void release() {}
};

// The composition root of the offline engine: reads a feedlog, runs each
// record through parse -> normalize -> unified book -> analytics -> api, and
// times the whole chain per record with the network already stripped away.
// One harness runs one file; make a fresh harness per replay.
class ReplayHarness {
 public:
  // session may be null when nothing consumes updates (pure benchmarking);
  // fields published per event update: kalshi_mid, poly_mid, basis.
  // book_mr backs the per-event order books and must outlive the harness.
  ReplayHarness(const normalize::ContractRegistry& registry,
                api::InProcessSession* session = nullptr,
                analytics::LeadLagConfig lead_lag_config = {},
                std::pmr::memory_resource* book_mr =
                    std::pmr::get_default_resource());

  // Backs each message's parse transients; null means the default resource.
  // Must outlive run().
  void set_parse_arena(ParseArena* arena) { parse_arena_ = arena; }

  // Time parse and the downstream pipeline separately. Off by default so
  // the headline latency stays a single clock bracket; the extra per-stage
  // clock read inflates the total, so a breakdown run is for the relative
  // split, not for the absolute latency number.
  void set_breakdown(bool on) { breakdown_ = on; }

  // Replay at the capture's own arrival schedule, time-compressed by
  // `speed`: 1.0 is real time, 100.0 is a hundred times the rate the
  // venue actually sent at, 0 (the default) is flat out and unpaced.
  //
  // Pacing is what makes response time measurable at all. It is also the
  // only way to ask where the engine stops keeping up: raise the speed
  // until response time detaches from service time, and that multiple of
  // the venue's real rate is the saturation point.
  void set_replay_speed(double speed) { replay_speed_ = speed; }

  // How much of each wait is spun rather than slept, in nanoseconds.
  //
  // This is the accuracy/CPU dial, and it has to be a dial because the
  // right setting depends on what is being measured. Sleeping is cheap
  // and inaccurate: a general-purpose OS overshoots by milliseconds under
  // load, which swamps a microsecond-scale response time. Spinning is
  // exact and costs a core for the whole run. Default 2 ms covers the
  // timer granularity; set it past the capture's largest gap to spin
  // every wait and take the instrument's error out of the answer.
  void set_pace_spin_ns(std::int64_t ns) { pace_spin_ns_ = ns; }

  // Replays at max rate. Nullopt only if the file cannot be opened.
  std::optional<ReplayStats> run(const std::string& feedlog_path,
                                 std::string* error = nullptr);

 private:
  void observe_basket(const std::string& event_id,
                      const model::BookDelta& delta);
  // Evaluates the complementary no-arbitrage bound for every event this
  // message touched. Deliberately called once per WIRE MESSAGE rather
  // than per delta: one Polymarket price_change carries several level
  // changes, and between "add the new best bid" and "remove the old best
  // ask" the folded book is transiently crossed. Sampling per delta
  // reports those transients as arbitrage that no one could ever have
  // traded, because the state never existed between messages.
  void observe_complementary_at_message_end();
  void on_event_update(const std::string& event_id,
                       const model::UnifiedBook& book,
                       const model::BookDelta& delta);

  const normalize::ContractRegistry& registry_;
  api::InProcessSession* session_;
  analytics::LeadLagConfig lead_lag_config_;

  feed::KalshiParser kalshi_;
  feed::PolymarketParser polymarket_;
  normalize::Normalizer normalizer_;
  ParseArena* parse_arena_ = nullptr;
  bool breakdown_ = false;

  struct EventAnalytics {
    analytics::DivergenceTracker divergence;
    // DivergenceTracker is a generic running-stats accumulator; reused here
    // to carry the mean/min/max bid-ask spread for each venue.
    analytics::DivergenceTracker kalshi_spread;
    analytics::DivergenceTracker poly_spread;
    analytics::DivergenceTracker kalshi_imbalance;
    analytics::DivergenceTracker poly_imbalance;
    analytics::DivergenceTracker kalshi_micro_gap;
    analytics::DivergenceTracker poly_micro_gap;
    analytics::DivergenceTracker micro_basis;
    analytics::DivergenceTracker internal_cross;
    std::uint64_t internal_two_sided = 0;
    std::uint64_t two_sided_updates = 0;
    std::uint64_t crossable_updates = 0;
    // Crossed-run state: a run opens on the first crossed update after an
    // uncrossed (or initial) one and closes on the next uncrossed update.
    // The open run is always episodes.back(); count and longest span are
    // derived from the records at report time rather than kept alongside.
    bool in_cross = false;
    // Running stats over the crossed depth (cents) and the executable
    // touch notional (dollars) of crossable updates; the same generic
    // accumulator the spreads use.
    analytics::DivergenceTracker cross_depth;
    analytics::DivergenceTracker cross_edge;
    analytics::DivergenceTracker cross_sweep;
    analytics::DivergenceTracker cross_net_edge;
    std::uint64_t profitable_updates = 0;
    analytics::DivergenceTracker cross_net_sweep;
    std::uint64_t sweepable_updates = 0;
    // Optimal net sweep as of the previous crossed update of the OPEN
    // episode: the standing value the survival fill logic reads when an
    // update crosses a tau boundary. Scratch for the harness, which is
    // why it lives here and not in the reported CrossedEpisode.
    double open_episode_last_sweep = 0.0;
    // Per-episode records; the open episode is always episodes.back().
    std::vector<ReplayStats::CrossedEpisode> episodes;
    // Last update receive time per venue, for quote-age at sample time.
    std::int64_t last_kalshi_ns = 0;
    std::int64_t last_poly_ns = 0;
    std::uint64_t stale_basis_samples = 0;
    std::int64_t stalest_quote_ns = 0;
    analytics::CrossCorrelationEstimator lead_lag;
    analytics::EventStudyEstimator event_study;

    explicit EventAnalytics(const analytics::LeadLagConfig& config)
        : lead_lag(config) {}
  };
  std::unordered_map<std::string, EventAnalytics> analytics_;
  // Basket bookkeeping: member lists from the registry, an index from
  // event id to basket, and per-venue running stats.
  struct BasketState {
    std::string id;
    std::vector<std::string> members;
    analytics::DivergenceTracker mid_sum[2];   // [0] kalshi, [1] polymarket
    double bid_sum_max[2] = {0.0, 0.0};
    std::uint64_t samples[2] = {0, 0};
    std::uint64_t max_two_sided[2] = {0, 0};
  };
  std::vector<BasketState> baskets_;
  // Events touched by the message currently being applied.
  std::vector<std::string> dirty_events_;
  std::unordered_map<std::string, std::size_t> event_to_basket_;
  LatencyRecorder latency_;
  LatencyRecorder response_latency_;
  double replay_speed_ = 0.0;
  std::int64_t pace_spin_ns_ = 2'000'000;  // 2 ms
  ReplayStats stats_;
};

}  // namespace basis::bench
