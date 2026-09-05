# docs/bench

Committed benchmark captures and the numbers they produce. Populated from
Phase 4 onward (replay harness + latency histogram) and Phase 5 (lead-lag).

Every figure quoted in the top-level README or on a resume must be reproducible
from an artifact in this directory plus a documented command, the same rule the
companion voxel-engine project follows. No aspirational numbers.

## What is here

| doc | question it answers | input |
|:--|:--|:--|
| [`allocator.md`](allocator.md) | Allocator benchmark: global heap vs Bloomberg bdlma | live-poly-60s |
| [`book_reconstruction.md`](book_reconstruction.md) | Reconstructing a book, and proving it is the venue's book | btcusdt-recon |
| [`cross_venue_lead.md`](cross_venue_lead.md) | Cross-venue lead: Binance against Coinbase | btc-xvenue, btc-xvenue-2 |
| [`economics.md`](economics.md) | Crossed-book economics: methodology and numbers | synthetic |
| [`extrapolation.md`](extrapolation.md) | Can a short capture stand in for a long one? | soak |
| [`fanout.md`](fanout.md) | Subscription fan-out with slow consumers | synthetic |
| [`ingest.md`](ingest.md) | Ingest throughput on a venue that produces load | binance-90s |
| [`latency.md`](latency.md) | Ingest-to-signal latency on a recorded live session | live-poly-30min |
| [`matching_engine.md`](matching_engine.md) | Matching engine: design, correctness, throughput | synthetic |
| [`soak.md`](soak.md) | Soak: four hours live, three real disconnects, nothing lost | soak |

`scripts/check_bench_inputs.py` runs in CI and fails if any doc here names
a capture that git does not track, because a benchmark whose input is
gitignored works perfectly for its author and for nobody else - and
running it is exactly what makes that look fine.

`scripts/extrapolate.py` answers the question that sits underneath all of
these: whether a figure measured over a short window may be quoted for a
longer one. On this data the median may and the tail may not, by a factor
of 63 (`extrapolation.md`), which is why the percentiles above come from
the thirty-minute and four-hour captures rather than a convenient slice.
