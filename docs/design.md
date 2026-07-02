# Engine design: the replay-driven pipeline

How the end-to-end engine is put together. PLAN.md owns scope and phases;
docs/api_integration.md owns the venue wire formats. This file owns the code:
module responsibilities, data flow, and the decisions that are not obvious
from the headers.

## Guiding decision: offline-first

The full pipeline (parse -> normalize -> unified book -> analytics -> api) is
built and verified against *recorded* feeds first. Live WebSocket adapters
slot in behind the same `FeedAdapter` interface later.

Why:

- Deterministic. A recorded session replays byte-identical, so every latency
  and lead-lag number is reproducible and CI-gateable.
- Testable without credentials. Kalshi requires an authenticated session even
  for public market data; the replay path needs no keys.
- It is the measurement story anyway. PLAN.md's latency numbers are defined
  as replay measurements with network jitter removed, so the replay path is
  the product, not a workaround.

## Data flow

```
capture file (.feedlog)
      |
      v
feed::FeedLogReader        one record = (recv_ns, venue, raw venue JSON)
      |
      v
feed::KalshiParser /       venue JSON -> canonical model::BookDelta
feed::PolymarketParser     (prices to cents, sides unified, gaps flagged)
      |
      v
normalize::Normalizer      venue market id -> event id (ContractRegistry),
      |                    routes delta into that event's UnifiedBook
      v
model::UnifiedBook         per-event: one OrderBook per venue, basis = mid_k - mid_p
      |
      v
analytics::DivergenceTracker   basis stats per event
analytics::LeadLagEstimator    which venue moves first (cross-correlation)
      |
      v
api::InProcessSession      BLPAPI-style push: (event, field) -> handlers
```

The replay harness (`bench::ReplayHarness`) drives this loop and timestamps
each record before parse and after publish; the difference is the
ingest-to-signal latency PLAN.md talks about.

## Module notes

### model

- `BookDelta` carries an `Action`:
  - `Set` - absolute size at a level (0 removes). Polymarket semantics.
  - `Add` - size change at a level. Kalshi `orderbook_delta` semantics.
  - `Clear` - drop the whole book. Emitted at the head of every snapshot and
    on gap recovery, so a book is never a mix of old and new state.
  `OrderBook::apply` is the only place these semantics live.
- `UnifiedBook` is one event's cross-venue view: an `OrderBook` per venue,
  `mid(venue)`, and `basis()` (Kalshi mid minus Polymarket mid, in cents).
  It is plain data; no callbacks, no threads.

### feed

- Parsers are stateless per message except Kalshi's per-market sequence
  tracking (needed to detect delta gaps). They take one raw WS message and
  return `ParseResult { deltas, status, gap }`.
  - status `Ok` / `Ignored` (valid but irrelevant message type, e.g.
    subscription acks) / `Malformed`.
  - `gap = true` tells the caller the venue book is untrustworthy; the parser
    already prepends a `Clear` so downstream state cannot go stale silently.
- Kalshi book semantics: the wire gives `yes` and `no` arrays of
  [price_cents, size]. A resting NO bid at p is a YES ask at 100 - p, so the
  parser folds both into one YES-frame book: `yes` -> bids, `no` -> asks at
  100 - p. This is exactly the kind of venue asymmetry the normalize layer
  exists for, and it is documented once, here, and implemented once, in the
  parser.
- Polymarket prices arrive as decimal probability strings ("0.47"); the
  parser converts to integer cents. Sizes are absolute (`Set`), and "0"
  removes a level. `price_change` carries BUY/SELL which map to Bid/Ask.
- `FeedLogReader` / `FeedLogWriter` own the capture format (below).

### normalize

- `TomlContractRegistry` reads `configs/contracts.toml`. It parses only the
  subset the registry needs ([[event]] tables of key = "string" pairs), which
  keeps a TOML library out of the dependency set. Unknown keys are ignored;
  malformed lines fail loudly with the line number.
- `Normalizer` owns the map event_id -> UnifiedBook. On each delta it looks
  up the event (unmapped markets are counted and skipped, never guessed),
  applies the delta, and notifies an observer callback with the updated
  event. Analytics and the API layer subscribe there; parsers never see
  events, analytics never see venue ids.

### analytics

- `DivergenceTracker`: running count / mean / min / max of basis per event.
- `LeadLagEstimator` (cross-correlation implementation):
  1. `observe(kalshi_mid, poly_mid, ts_ns)` appends a sample whenever both
     mids exist.
  2. `estimate()` resamples both series onto a fixed grid (default 100 ms)
     with last-observation-carried-forward, diffs them into return series,
     and computes Pearson correlation of `r_a[t]` vs `r_b[t + k]` for lags
     k in [-K, +K].
  3. The k maximizing correlation is the lag of venue B behind venue A;
     `lead_seconds = k * grid_dt` (positive means A leads).
  Reported with the peak correlation and sample count, per PLAN.md's
  "observational, never causal" rule. The estimator is validated by a test
  that generates series with a known injected lead and checks it is
  recovered.

### api

- `InProcessSession`: mutex-guarded subscription table, synchronous publish
  on the caller's thread. Fields published per event update: `kalshi_mid`,
  `poly_mid`, `basis`. Good enough for the offline engine; a queued async
  session arrives with the live feeds.

### bench

- `LatencyRecorder`: stores raw nanosecond samples, reports
  min/p50/p90/p99/max/mean. Exact percentiles from a sort at report time; an
  HdrHistogram swap is a Phase-4 option if sample volume ever makes storing
  samples silly.
- `ReplayHarness`: the composition root. Reads a feedlog, runs the pipeline,
  collects per-message latency and message/gap/unmapped counters, and prints
  a report. Replay is max-rate (as fast as the engine drains), which is what
  the latency measurement wants.

## Capture format (.feedlog)

One record per line, tab-separated:

```
recv_ns <TAB> venue <TAB> raw venue JSON (verbatim, single line)
```

- `recv_ns`: receive timestamp, ns since epoch. During live capture this is
  the socket-read time; during synthesis it is the simulated clock.
- `venue`: `kalshi` or `polymarket`.
- The payload is stored verbatim so replay exercises the same parser code the
  live path will use. Anything after the second tab is payload, so tabs
  inside JSON strings are safe.

## Synthetic sessions (tools story)

`basis synth` generates a deterministic session: a latent probability random
walk (fixed-seed mt19937), quoted by a synthetic Kalshi immediately and by a
synthetic Polymarket after a configurable delay, serialized as real venue
wire JSON into a feedlog. Because the lead is injected, `basis replay` on
that file must report it back; that closed loop validates the whole engine
end to end without any network.

Synthetic numbers are for validation only. They never appear in README/PLAN
claims; those wait for recorded live sessions (docs/bench rule).

## Threading

The replay path is single-threaded on purpose: deterministic, and the
latency being measured is per-message compute, not queueing. The live path
(Phase 1/2) adds one IO thread per venue socket feeding a bounded queue
drained by the analytics thread; that boundary is the only planned crossing
and it lands with the code that needs it, not speculatively.

## Error policy

No exceptions on the hot path. Parsers return status enums; file and config
loaders return `std::optional` or a result struct with an error string.
Malformed input is counted and reported, never silently dropped (PLAN.md's
reliability rule: every dropped message is a number, not a hope).
