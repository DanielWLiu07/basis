# Ingest-to-signal latency on a recorded live session

Measured 2026-07-04 at commit e3d352b on an Apple M4 (Apple clang 21,
Release, macOS). The input is a committed artifact, so the numbers are
auditable, not just repeatable in spirit.

## The session

`live-poly-30min.feedlog.gz` (in this directory) is a 30 minute capture of
Polymarket's public market channel for every contract in
`configs/contracts.toml` (14 events: 2026 World Cup winners and July 2026
Fed decision), recorded on a World Cup weekend:

- 34,731 messages, 28 MB raw, zero malformed, zero rejected writes,
  zero reconnects
- 266,597 canonical deltas after normalization, zero unmapped
  (NO-token folding on; both wire views of each book land in one frame)

Reproduce the capture side with `basis record` (a new session records
current market activity; the committed artifact is what the numbers below
came from):

```
gunzip -k docs/bench/live-poly-30min.feedlog.gz
./build/src/basis replay docs/bench/live-poly-30min.feedlog \
    --config configs/contracts.toml
```

## Results

Five consecutive replays, same binary, quiet machine. The measured span
per record is parse -> normalize -> analytics -> publish; the feedlog read
stands in for the network and stays outside it.

| run | p50    | p90    | p99     | throughput        |
| --- | ------ | ------ | ------- | ----------------- |
| 1   | 0.5 us | 0.7 us | 38.0 us | 818k records/sec  |
| 2   | 0.5 us | 0.7 us | 37.1 us | 856k records/sec  |
| 3   | 0.5 us | 0.6 us | 37.1 us | 862k records/sec  |
| 4   | 0.5 us | 0.7 us | 37.3 us | 824k records/sec  |
| 5   | 0.5 us | 0.7 us | 36.9 us | 847k records/sec  |

Allocation profile (`--alloc count`): 2.1 parse allocations and 1,021 B
per message, 1.5 book-node allocations per message.

## Reading the numbers

- The p99 is stable across runs, so it is structural, not scheduler
  noise: the tail is Polymarket `book` snapshots, which carry hundreds of
  price levels in one message and rebuild a whole book. The median
  message is a small `price_change`.
- Real-session throughput (about 840k records/sec) is lower than the
  synthetic benchmark's 3.7M records/sec because the messages are about
  4x larger (840 B vs 180 B average) and fan out to 7.7 deltas per
  message instead of 0.5. Both numbers are honest; they describe
  different message mixes.
- A 60 second capture was tried first and rejected for this document:
  877 records gave p99 swings of 3 to 13 us between identical runs.
  Percentiles need the larger sample.
