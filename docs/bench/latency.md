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

## Service time is not response time

Everything above is **service time**: the clock starts when the engine
picks a record up. Replayed flat out that is all it can ever be. The loop
does not move on until the previous record is finished, so a slow record
delays its successors and nobody measures the delay - and the slower a
record is, the fewer of its successors get sampled during the stall. That
is coordinated omission, and it flatters a p99 in exactly the situation a
consumer would be suffering.

The capture knows when each message actually arrived. `--speed N` replays
against that schedule, compressed N times, and measures **response time**
from when each record was due rather than from when the engine got to it:

```
# realistic idling, honest service time, response time unresolvable
./build/src/basis replay docs/bench/live-poly-30min.feedlog \
    --config configs/contracts.toml --speed 1

# accurate schedule, response time resolved, one core pinned for 30 min
./build/src/basis replay docs/bench/live-poly-30min.feedlog \
    --config configs/contracts.toml --speed 1 --pace-spin-ms 3000
```

### Why it matters here: the feed is not smooth

The session averages 19.3 messages/sec over half an hour, which is the
number the "four orders of magnitude of headroom" claim divides into the
engine's throughput. The instantaneous rate is nothing like that:

| inter-arrival gap | |
| --- | ---: |
| p1 | 1 us |
| p10 | 2 us |
| p50 | 2.8 ms |
| p90 | 151 ms |
| p99 | 610 ms |
| max | 2,148 ms |

41% of the gaps in this capture are under 100 microseconds and 10% are
under 2. Polymarket does not trickle; it delivers a batch and goes quiet.
Inside a batch the arrival rate is on the order of a million messages a
second against an engine measured at 441k-860k, and a consumer genuinely
cannot act on the fiftieth message of a segment until it has processed the
forty-nine in front of it. That queueing is real, it is what response time
counts, and service time cannot see it by construction.

### What the schedule costs before any queueing at all

The first thing pacing showed was not coordinated omission. It was that
the service time itself changes:

| service time | unpaced (back to back) | paced at 1x, sleeping between records |
| :--- | ---: | ---: |
| p50 | 0.8 us | **14.4 us** |
| p90 | 0.9 us | 46.3 us |
| p99 | 77.5 us | 490.8 us |
| throughput | 458k rec/s | 33k rec/s |

Same binary, same records, same work per record, one session. The only
difference is that the paced run waits between them the way the venue
actually delivered them, and the median is **18x higher**.

A benchmark loop keeps the caches warm, the branch predictors trained and
the core clocked up. A feed that sends 19 messages a second and then goes
quiet does none of that, so every record arrives cold. This has nothing to
do with coordinated omission - it is a second thing an unpaced benchmark
hides, and unlike coordinated omission it moves the median more than the
tail. The figure this document published from July describes a machine
doing nothing but replaying this file back to back.

### The harness measures its own error too

Pacing means sleeping, and a sleep on a general-purpose OS overshoots. If
the harness charged its own overshoot to the engine the whole exercise
would be worthless, so the two are separated: a record is counted as
**engine backlog** only when the previous record was still being processed
when this one came due, and as **pacer overshoot** when the engine was
idle and the harness simply woke late. The run reports both.

Overshoot is bounded by spinning the last 2 ms of every wait and capping
each sleep at 20 ms, but on a laptop it still reaches milliseconds when
the process is descheduled. Response-time percentiles are therefore an
upper bound: the backlog counts, which are overshoot-free by
construction, are the numbers to read.

### How much the p99 was flattering itself

Accurate pacing means spinning rather than sleeping, because a sleep on a
general-purpose OS overshoots by milliseconds and the quantity here is
microseconds. `--pace-spin-ms 3000` spins every wait in this capture. It
costs a core for the whole half hour, and it buys a run where service and
response time are measured under identical conditions, so the difference
between them is coordinated omission and nothing else:

| spin-paced at 1x | service | response | ratio |
| :--- | ---: | ---: | ---: |
| p50 | 1.9 us | 3.8 us | 2.0x |
| p90 | 13.4 us | 67.7 us | 5.1x |
| p99 | 115.7 us | **1,533.9 us** | **13.3x** |

CI gates the relationship rather than the microseconds. `perf_gate.sh`
paces a short synthetic session at 1x on every commit and fails the build
unless `response p99 >= service p99`. Absolute latency on a shared runner
is noise; the invariant is not - response time is service time plus a
wait, and a wait cannot be negative, so that inequality holds on every
machine that has ever existed. If it ever fails, the harness is reading
the wrong clock and every number in this file is suspect.

**The p99 an unpaced replay reports is optimistic by more than an order of
magnitude** against what a consumer of the same feed at the same rate
actually waits. The median barely moves, which is the shape coordinated
omission always has: it hides in the tail, because the tail is exactly
where the stall that suppressed the sampling happened.

The engine is genuinely behind on **14.8% of records** - roughly one in
seven - at the venue's own delivery rate. Not on average: the average is
19 messages a second against a pipeline that does 458k. Inside a batch.

Two caveats, both stated rather than smoothed:

- The pacer still overshot on 10.8% of records, so the response
  percentiles are an upper bound. The worst overshoot (52 ms) has a twin
  in the same run's service-time maximum (49 ms), which means it is the
  OS descheduling the process, not the pacing design. Maxima from this
  mode are machine noise and are not quoted.
- Spinning keeps the core hot, so the service column here (p50 1.9 us) is
  close to the unpaced 0.8 us rather than the cold 14.4 us. That is the
  trade: **an accurate pacer and a realistic idle pattern are mutually
  exclusive**, because how the harness waits changes what the engine's
  caches look like when the next record lands. Sleeping gives the honest
  cold service time and cannot resolve response time; spinning resolves
  response time on an unrealistically warm engine. The two runs bracket a
  real consumer rather than either one being it, and a real consumer is
  worse than both, because it idles cold *and* queues.

### Compressing time is a stress test, not a forecast

`--speed 1000` does not simulate a venue a thousand times busier. It
multiplies the gaps *inside* each batch by the same factor as the gaps
between them, so it manufactures an instantaneous rate no venue produced.
It is useful as an overload probe and is labelled as one; the only figure
that describes this venue is `--speed 1`.

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
