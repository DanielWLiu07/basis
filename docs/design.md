# Engine design: the replay-driven pipeline

How the end-to-end engine is put together. PLAN.md owns scope and phases;
docs/api_integration.md owns the venue wire formats. This file owns the code:
module responsibilities, data flow, and the decisions that are not obvious
from the headers.

## Guiding decision: offline-first

The full pipeline (parse -> normalize -> unified book -> analytics -> api)
was built and verified against *recorded* feeds first. The live WebSocket
adapters (`feed::PolymarketFeed`, `feed::KalshiFeed`) sit behind the same
`FeedAdapter` interface and feed the same parsers; `basis record` captures
their raw stream into the same file format the replay path consumes.

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
analytics::DivergenceTracker          basis stats per event
analytics::CrossCorrelationEstimator  which venue moves first
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

- Parsers are stateless per message except Kalshi's sequence tracking,
  which is per subscription id (sid), not per market: one subscription
  carries interleaved markets on a single shared sequence. They take one
  raw WS message and return `ParseResult { deltas, status, gap, sid }`.
  - status `Ok` / `Ignored` (valid but irrelevant message type, e.g.
    subscription acks) / `Malformed`.
  - `gap = true` tells the caller the venue book is untrustworthy; the parser
    already prepends a `Clear` so downstream state cannot go stale silently.
    The live Kalshi feed answers a gap by unsubscribing that sid and
    subscribing again, which forces a fresh snapshot.
  - The result is arena-friendly: `deltas` is a pmr vector backed by the
    memory_resource handed to `parse()`, and `BookDelta::market` is a view
    into the simdjson buffer (valid until the next parse on the same
    parser), so the hot path copies no strings. Anything that outlives the
    message, like the Kalshi snapshot ledger, copies what it keeps.
- Both venues express the NO side of a binary market as a mirror of the
  YES side (a NO bid at p is a YES ask at 100 - p), but on different
  layers, so the fold lives in two places on purpose:
  - Kalshi sends `yes` and `no` arrays inside one message, so the parser
    folds them into one YES-frame book (`yes` -> bids, `no` -> asks at
    100 - p).
  - Polymarket sends the NO-outcome token's book as separate messages, and
    only the registry knows which tokens are NO tokens, so the normalizer
    does that fold when routing the delta.
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
  folds Polymarket NO-token deltas into the YES frame, applies the delta,
  and notifies an observer callback with the updated event. Analytics and
  the API layer subscribe there; parsers never see events, analytics never
  see venue ids.
- The per-delta lookup allocates nothing: registry maps use a transparent
  hash so a string_view probes them directly, and `event_id()` returns a
  view over registry-owned storage. The book maps draw their nodes from an
  injectable memory_resource (see bench).

### analytics

- `DivergenceTracker`: running count / mean / min / max of basis per event.
- `CrossCorrelationEstimator`:
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
- The point estimate ships with a 95 percent confidence interval from a
  paired moving-block bootstrap: whole blocks of (a, b) return pairs are
  resampled together, preserving each series' autocorrelation and the
  cross-venue timing, and the per-resample peak search stays near the
  full-sample peak (an interval for the located lead, not a fresh global
  search). Fixed seed through core/rng.h, so the interval is reproducible.
- `EventStudyEstimator` is the independent cross-check the methodology
  calls for: a move is a repricing of at least a threshold from a running
  reference, a follow is the other venue moving the same way within a
  window, reported as median follow time with matched counts, plus the
  mirror direction as the control. Two unrelated methods agreeing on the
  lead is what makes the finding defensible; the closed-loop test requires
  both to recover the injected lead.

### api

- `InProcessSession`: mutex-guarded subscription table, synchronous publish
  on the caller's thread. Fields published per event update: `kalshi_mid`,
  `poly_mid`, `basis`. Good enough for replay-driven consumers; a queued
  async session is future work if a consumer ever needs to get off the
  publishing thread.

### bench

- `LatencyRecorder`: stores raw nanosecond samples, reports
  min/p50/p90/p99/max/mean. Exact percentiles from a sort at report time; an
  HdrHistogram swap is a Phase-4 option if sample volume ever makes storing
  samples silly.
- `ReplayHarness`: the composition root. Reads a feedlog, runs the pipeline,
  collects per-message latency and message/gap/unmapped counters, and prints
  a report. Replay is max-rate (as fast as the engine drains), which is what
  the latency measurement wants.
- The harness exposes the two allocation seams the hot path has: a
  `ParseArena` (per-message transients, released after each message) and a
  book memory_resource (long-lived node churn). `replay --alloc` selects
  the global heap, a counting wrapper that reports heap traffic per
  message, or Bloomberg bdlma arenas; docs/bench/allocator.md records what
  those measured and why the heap default shipped.

### net (BASIS_ENABLE_NET)

- `WsClient`: one TLS WebSocket on its own IO thread. Reconnect with
  exponential backoff plus jitter; subscriptions are re-sent by the
  connect handler after every reconnect, and upgrade headers can be minted
  per connection (Kalshi's auth signature embeds a timestamp, so a stale
  header set would be rejected on reconnect).
- `KalshiSigner`: RSA-PSS / SHA-256 over timestamp + method + path, key
  loaded from a gitignored PEM. The single TU that touches OpenSSL's EVP
  API for signing; verified offline by tests against the public key half.
- The feed adapters own the venue protocol on top: subscribe message
  shape, gap recovery, malformed/delta counters, and a raw tap that
  `basis record` uses to write the feedlog.

### alloc (BASIS_ENABLE_BDE)

- Wrappers exposing Bloomberg bdlma allocators as std::pmr resources.
  One TU compiled as C++17 to match Homebrew's BDE archives (bsls coerces
  the dialect at link time); the rest of the engine sees only
  `std::pmr::memory_resource`, whose ABI does not vary by dialect.

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
runs one IO thread per venue socket (`WsClient`); parsing and the delta
sink run on that thread. `basis record` serializes the two venues' raw
taps into one feedlog under a mutex. `basis live` runs analytics in real
time: the IO threads push owning copies of each delta (`OwnedBookDelta`;
a queued view into the parser buffer would dangle) into one
`core::BoundedQueue` drained by a single analytics thread that owns the
normalizer and per-event trackers. The queue blocks when full rather than
dropping, bursts back up into TCP, and its counters (in, out, high water,
blocked pushes) are printed at exit so zero loss across the boundary is a
number, not a hope. Quotable latency numbers still come from replay,
where the network is stripped away.

## Error policy

No exceptions on the hot path. Parsers return status enums; file and config
loaders return `std::optional` or a result struct with an error string.
Malformed input is counted and reported, never silently dropped (PLAN.md's
reliability rule: every dropped message is a number, not a hope).
