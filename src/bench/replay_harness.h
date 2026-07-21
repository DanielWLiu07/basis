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
    analytics::LeadLagResult lead_lag;      // positive: Kalshi leads
    analytics::EventStudyResult event_study;  // independent cross-check
  };
  std::vector<EventReport> events;  // sorted by event id
};

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

  // Replays at max rate. Nullopt only if the file cannot be opened.
  std::optional<ReplayStats> run(const std::string& feedlog_path,
                                 std::string* error = nullptr);

 private:
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
    std::uint64_t two_sided_updates = 0;
    std::uint64_t crossable_updates = 0;
    // Crossed-run state: a run opens on the first crossed update after an
    // uncrossed (or initial) one and closes on the next uncrossed update.
    bool in_cross = false;
    std::int64_t cross_start_ns = 0;
    std::uint64_t crossable_episodes = 0;
    std::int64_t crossable_longest_ns = 0;
    // Running stats over the crossed depth (cents) of crossable updates;
    // the same generic accumulator the spreads use.
    analytics::DivergenceTracker cross_depth;
    analytics::CrossCorrelationEstimator lead_lag;
    analytics::EventStudyEstimator event_study;

    explicit EventAnalytics(const analytics::LeadLagConfig& config)
        : lead_lag(config) {}
  };
  std::unordered_map<std::string, EventAnalytics> analytics_;
  LatencyRecorder latency_;
  ReplayStats stats_;
};

}  // namespace basis::bench
