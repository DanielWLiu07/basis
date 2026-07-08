# Soak: four hours live, three real disconnects, nothing lost

Measured 2026-07-07 at commit d5e3ef4 on an Apple M4 (Apple clang 21,
Release, macOS). The input is the committed artifact
`soak.feedlog.gz` in this directory, so every number below is auditable.

## The session

A four hour unattended `basis record` of Polymarket's market channel for
all 14 registry contracts (12,833 seconds between first and last
message):

- 59,891 messages, 49 MB raw, zero malformed, zero rejected writes
- 3 natural venue disconnects, each survived by the reconnect path
  (exponential backoff, re-subscribe, snapshot recovery); the recording
  simply continues on the other side of each drop
- Replay accounting: 494,545 deltas applied, zero unmapped, zero gaps,
  zero malformed lines
- Integrity: 37 snapshot hashes verified, zero mismatched, across the
  four connection generations (initial subscribe plus three recoveries);
  the venue's refresh-form snapshots (1,302) omit hashed fields and are
  counted as unverifiable, per docs/api_integration.md

```
gunzip -k docs/bench/soak.feedlog.gz
./build/src/basis replay docs/bench/soak.feedlog --config configs/contracts.toml
```

## Latency, cross-checked against the shorter capture

Three consecutive replays:

| run | p50    | p90    | p99     | throughput        |
| --- | ------ | ------ | ------- | ----------------- |
| 1   | 0.5 us | 0.7 us | 36.5 us | 758k records/sec  |
| 2   | 0.5 us | 0.7 us | 36.2 us | 770k records/sec  |
| 3   | 0.5 us | 0.7 us | 37.0 us | 771k records/sec  |

The percentiles agree with `latency.md`'s 30 minute capture (p50 0.5 us,
p99 37 us) on a session recorded three days later with 1.7x the message
count: the latency numbers are properties of the engine, not of one
lucky recording.

## What this claims and what it does not

This is the reliability requirement in PLAN.md exercised by the real
world rather than by the fault-injection test: unattended hours, real
venue drops, and an accounting trail in which every message is a number.
It says nothing about the cross-venue lead; that still waits on Kalshi
credentials.
