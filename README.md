# basis

**A C++20 market-data engine that streams live order books from four
trading venues, merges them into one view, and measures which venue's
price moves first.**

It connects to Kalshi, Polymarket, Binance and Coinbase over TLS
WebSocket, rebuilds each venue's order book from its feed, normalizes all
four into one schema, and serves the result through a Bloomberg
BLPAPI-style interface: push subscriptions and pull requests, both
checked against per-user entitlements.

The question it was built to answer: when two venues quote the same
asset, which one reprices first, and is the gap real or an artifact of
how it was measured?

| | |
| :--- | :--- |
| **Headline result** | Binance leads Coinbase on BTC price discovery, z = 11.76 on one capture, replicated on a second (z = 24 to 39) and on ETH |
| **Throughput** | 2.15M venue messages/sec parsed and applied to books |
| **Stack** | C++20, CMake, simdjson, Boost.Beast + OpenSSL, GoogleTest, libFuzzer, Bloomberg BDE (optional) |
| **Checked on every commit** | Linux and macOS builds with warnings as errors, ASan/UBSan, TSan, fuzzing, a performance gate, a dependency-layering check |
| **Rule for every number below** | it regenerates from a capture committed to this repo, or it is labelled as not yet measured |

## Contents

- [The headline result](#the-headline-result)
- [Numbers](#numbers)
- [Try it in sixty seconds](#try-it-in-sixty-seconds)
- [How it fits together](#how-it-fits-together)
- [How it is kept honest](#how-it-is-kept-honest)
- [What it does not do yet](#what-it-does-not-do-yet)
- [Glossary](#glossary)
- [Build options](#build-options)
- [Read more](#read-more)

## The headline result

**When Bitcoin reprices on Binance, Coinbase follows far more often than
the other way round.**

| First move on | followed by the other venue within 2 s |
| :--- | ---: |
| Binance | **57.7%** of the time |
| Coinbase | 26.4% of the time |

A "repricing" is a mid-price move of at least $0.25; "followed" means the
other venue moved the same way inside a two second window. The gap is
significant at z = 11.76 (a two-proportion test) over 204,864 messages
from a 45 minute live capture, and it holds at every threshold tested,
from $0.25 up to $2.00.

It has been checked three ways since:

- **Replication.** A second 45 minute capture eight days later (642,919
  messages) gives the same ordering at every threshold, z = 24 to 39.
- **A second instrument.** A 29 minute capture of BTC and ETH on both
  venues at once gives the same ordering on each (z = 5.19 and 9.43), so
  ETH acts as a control on BTC under identical market conditions.
- **The measurement is biased against it.** The capture host sits about
  86 ms further from Binance than from Coinbase one-way, which stamps
  Binance's moves late and makes Binance look like the follower. Coinbase's
  50 ms update batching pushes the other way, so the net bias is 36 to
  86 ms against finding a Binance lead. The ordering shows up anyway.

What it does **not** claim, and why:

- **Not the percentages themselves.** On the busier second capture the
  follow rates roughly doubled (90.3% and 53.9%), because a two second
  window catches more coincidence in a fast market. The ordering and its
  significance replicate; the rates describe one session.
- **Not a lead in milliseconds.** The cross-correlation estimator's
  confidence interval touches zero and is reported as unresolved. A
  grid-free Hayashi-Yoshida estimator leans the same way only once the
  86 ms network offset is accounted for, and that offset was measured out
  of band, so it cannot be subtracted and quoted.
- **Not causal, and not tradeable.** A lead seen from one laptop says
  nothing about what a colocated trader could capture.

The capture is committed, so the result regenerates offline:

```
gunzip -c docs/bench/btc-xvenue.feedlog.gz > /tmp/btc-xvenue.feedlog
./build/src/basis xvenue-lead /tmp/btc-xvenue.feedlog --move-cents 25
```

Method, controls, and what would falsify it:
[`docs/bench/cross_venue_lead.md`](docs/bench/cross_venue_lead.md).

## Numbers

Each row links to the writeup that produced it, including the command
that regenerates it from a committed capture.

**Research results**

| Measured | Result | Where |
|---|---|---|
| **Cross-venue price discovery**: which venue reprices first | Binance repricings followed 57.7% of the time, Coinbase 26.4% (z = 11.76, 204,864 messages) | [`cross_venue_lead.md`](docs/bench/cross_venue_lead.md) |
| **Cross-venue arbitrage**: Kalshi against Polymarket on the September 2026 FOMC decision | books cross on 59.4% of updates; 0 of 1,383 crossings profitable after Kalshi's taker fee | [`cross_venue_fomc.md`](docs/bench/cross_venue_fomc.md) |

**Performance**

| Measured | Result | Where |
|---|---|---|
| **Ingest throughput**: raw venue JSON parsed and applied to books | 2.15M messages/sec, median of five runs (2.00 to 2.19M); 8.2M book deltas/sec | [`ingest.md`](docs/bench/ingest.md) |
| **Ingest-to-signal latency**: message in to analytic published, replayed back to back | p50 0.8 µs, p99 78 µs at 458k records/sec | [`latency.md`](docs/bench/latency.md) |
| **Coordinated omission**: the same tail, counting time spent waiting in line | the slowest 1% of records wait 13.3x their own processing time (116 µs of processing, 1,534 µs response) | [`latency.md`](docs/bench/latency.md) |
| **Conflated fan-out**: one publisher, 16 subscribers, some of them slow | the publisher runs 40 to 46x faster than inline fan-out | [`fanout.md`](docs/bench/fanout.md) |
| **Hot-path allocations** | 1 to 2 heap allocations per message; Bloomberg BDE arenas measured at parity with the heap | [`allocator.md`](docs/bench/allocator.md) |
| **Matching engine**: price-time-priority order book | 42.6M operations/sec at 23.5 ns/op, 2.1x a `std::map` book | [`matching_engine.md`](docs/bench/matching_engine.md) |

**Correctness and reliability**

| Measured | Result | Where |
|---|---|---|
| **Book reconstruction**: the rebuilt book against the venue's own REST snapshot | 40 of 40 levels match, zero mismatches | [`book_reconstruction.md`](docs/bench/book_reconstruction.md) |
| **Unattended soak**: left running live | 4 hours, 59,891 messages, 3 venue disconnects recovered, zero gaps | [`soak.md`](docs/bench/soak.md) |

Latency figures come from deterministic replay of a recorded session, so
they measure the engine's own compute with network jitter removed.
`scripts/bench.sh` regenerates the table from the committed captures
through `replay --json`, and `scripts/perf_gate.sh` fails CI if lead
recovery, the integrity counters, the allocation budget, or the
throughput floor regress.

## Try it in sixty seconds

No network and no credentials. Generate a synthetic session with a known
400 ms cross-venue lead injected, replay it through the real parsers, and
watch the engine report that lead back.

Needs CMake, Ninja and a C++20 compiler; GoogleTest and simdjson are
fetched by the configure step.

```
cmake -B build -G Ninja
cmake --build build -j
./build/src/basis synth captures/demo.feedlog --steps 5000 --lead-ms 400
./build/src/basis replay captures/demo.feedlog
```

Replay prints:

- **message accounting**, so nothing is ever silently dropped
- **per-event basis**, the price difference between the two venues
- **the recovered lead**, with a bootstrap confidence interval and an
  independent event-study cross-check
- **latency percentiles**

The same closed loop runs in CI: if the engine cannot recover the
injected lead through the real parsers, by both methods, the build is red.

![Synthetic session with the injected 400 ms cross-venue lead visible](docs/img/synth-lead.png)

One random walk quoted by two synthetic venues, one of them 400 ms
behind. Replay reports 0.400 s at correlation 1.00.

## How it fits together

```
  Kalshi ----+
  Polymarket +
  Binance ---+---> feed handlers ---> normalize ---> unified book
  Coinbase --+     (4 parsers,        (venue ids        (per event,
                   one adapter         become one        both venues)
                   seam)               event id)              |
                                                              v
                                                         analytics
                                          (basis, lead-lag, microprice,
                                           crossed-book economics net of
                                           fees and of reaction delay)
                                                              |
                                                              v
                                       BLPAPI-style API: subscription (push,
                                       conflated) + request (pull), both
                                       entitlement-checked, default-deny

  exec/ ---> price-time-priority matching engine, Gtc/Ioc/Fok
             re-executes the analytics' sweeps as order flow so the
             two must agree
```

**Feed handlers: reading each venue's dialect.** Four venue parsers sit
behind one adapter seam, and each turns its venue's JSON into one
canonical book delta. Parsing is zero-copy: market ids are views into
the simdjson buffer, and every allocation draws from an injectable
`std::pmr` resource. Sequence gaps are detected and answered by dropping
the book and re-requesting a snapshot, because a book known to be stale
must never be served.

**Normalization: one name per real-world outcome.** Kalshi names a
contract by ticker and Polymarket by token id. The contract registry maps
both to one venue-neutral event id, so nothing downstream ever sees a
venue id. Unmapped markets are counted and skipped, never guessed.

**Distribution: slow consumers get the latest price, not a backlog.**
Fan-out is conflated, keeping one slot per topic rather than a queue, so
memory is bounded by subscribers times topics rather than by publish rate.
Subscription pushes values as they change; request pulls the current
value immediately, which is what a screen needs when it first opens.

**Entitlements: who may see what.** Default-deny, granted per subscriber
and per topic, and enforced identically on push and pull, since a pull
path that skipped the check would be a way around it. A refusal looks
exactly like a topic that does not exist, so a caller cannot use denials
to enumerate what exists. Denials are counted rather than merely
prevented.

**Matching engine: the analytics, re-executed.** `exec/` is a
price-time-priority matching engine (Gtc, Ioc and Fok orders). Its ladder
is a flat array of 99 price slots rather than a tree, because
prediction-market prices are whole cents from 1 to 99, so best bid and
ask are a bit scan. When the analytics compute an arbitrage sweep
arithmetically, CI executes the same sweep through this engine as order
flow and requires the two to agree exactly.

## How it is kept honest

The CI workflow runs on every change and fails the build if any of these
break:

- **Build and tests on Linux and macOS**, with warnings as errors, plus a
  book-reconstruction check against a committed Binance REST snapshot.
- **Sanitizers.** The full test suite under AddressSanitizer with
  UndefinedBehaviorSanitizer, and separately under ThreadSanitizer.
- **Fuzzing.** libFuzzer runs against every untrusted-input surface: the
  Kalshi and Polymarket parsers and the contract registry.
- **Performance gate.** The injected lead must still be recovered by both
  methods, integrity counters must stay at zero, the hot path must stay
  within its allocation budget, a paced replay's p99 response time must
  be at least its p99 service time (the coordinated-omission invariant),
  and throughput must stay above a floor.
- **Physical design.** `scripts/levelize.py` builds the include graph and
  fails on a dependency cycle or on a dependency between libraries that
  is not declared, before anything compiles.
- **Docs checked against code.** One script fails if the docs mention a
  flag or subcommand the binary does not have; another fails if a
  benchmark writeup cites a capture that git does not track.

[`docs/postmortems.md`](docs/postmortems.md) covers five defects that got
past their own test suites anyway, and why nothing caught them.

## What it does not do yet

- **No trade prints in any capture.** `model::Trade` exists and the
  Coinbase parser emits trades, but the live feed subscribes to depth
  only, so no committed capture contains a trade and no analytic consumes
  one yet.
- **Prediction markets update slowly.** Kalshi sent 218 updates in 23.6
  minutes of the FOMC capture, and 59.7% of the crossing samples were
  priced against a quote more than 5 seconds old. So an unknown share of
  the 59.4% crossing rate is one book being stale rather than two live
  books disagreeing. That is why the lead-lag headline uses the crypto
  pair, which quotes continuously.

## Glossary

<details>
<summary>Terms used above, in plain language</summary>

- **Order book**: every resting buy order (bid) and sell order (ask) at
  each price. The **mid** is halfway between the best bid and best ask.
- **Snapshot, delta, sequence gap**: venues send a full book once, then a
  stream of numbered changes. A missing number means the book can no
  longer be trusted until a fresh snapshot arrives.
- **Basis**: the price difference for the same outcome on two venues;
  here, Kalshi's mid minus Polymarket's, in cents. The repo is named
  after it.
- **Lead-lag**: which of two venues tends to move first. Here it is
  observational, never causal.
- **z-score**: how many standard errors the gap between two rates is from
  zero. Above about 2 is unlikely to be chance; 11.76 is far above it.
- **Hayashi-Yoshida**: a way to correlate two prices that update at
  different, irregular times without first forcing them onto a common
  clock, which would throw away the timing being measured.
- **p50 / p99**: the median, and the value 99 of 100 measurements fall
  under. The p99 is the tail a trading system cares about.
- **Coordinated omission**: a benchmark that waits for each message to
  finish before sending the next never measures the time messages spend
  queued behind a slow one. Replaying at the capture's real pace exposes
  it.
- **Conflation**: keeping only the latest value per topic instead of
  queueing every update, so a slow reader skips stale prices.
- **BLPAPI**: Bloomberg's market-data API, which has a subscription
  (push) half and a request (pull) half. This project follows its design
  and uses Bloomberg's open-source BDE libraries; it uses no Bloomberg
  data.
- **Entitlements**: per-user permission to receive a given piece of data.
- **Taker fee**: the fee charged to an order that trades against a
  resting one.

</details>

## Build options

```
ctest --test-dir build --output-on-failure
```

The configure pulls GoogleTest and simdjson. With `-DBASIS_ENABLE_BDE=ON`
(`brew install bde` on macOS), `replay --alloc bde` runs the hot path on
Bloomberg `bdlma` arenas and `--alloc count` reports heap traffic per
message. `replay --breakdown` splits ingest-to-signal time into parse
versus everything downstream: on the synthetic session parse is about 60%
(simdjson plus canonicalization), the rest of the pipeline the other 40%.
Live capture needs `-DBASIS_ENABLE_NET=ON` (Boost and OpenSSL); see
[`docs/live_capture.md`](docs/live_capture.md).

## Read more

**Results and methods**
- [`docs/bench/`](docs/bench/): one writeup per measurement, each with
  the command that regenerates it from a committed capture
- [`docs/analytics_notes.md`](docs/analytics_notes.md): what the
  analytics compute, and the failures that shaped them

**How it is built**
- [`docs/design.md`](docs/design.md): the pipeline, module by module,
  plus the matching engine and the source layout
- [`docs/venues.md`](docs/venues.md): why this venue pairing, and which
  venues have actually streamed rather than been implemented
- [`PLAN.md`](PLAN.md): the full spec

**Why it survives contact with a real feed**
- [`docs/reliability.md`](docs/reliability.md): the socket failure that
  does not error, the reconnect tests, and the fuzzers
- [`docs/postmortems.md`](docs/postmortems.md): five defects that
  survived their own test suites, and why nothing caught them
- [`docs/live_capture.md`](docs/live_capture.md): recording your own,
  and the registry rot that makes an empty capture ambiguous
