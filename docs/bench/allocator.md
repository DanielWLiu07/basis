# Allocator benchmark: global heap vs Bloomberg bdlma

Measured 2026-07-02 at commit 61471f6 on an Apple M4 (Apple clang 21,
Release, macOS). BDE 4.38.0.0 from Homebrew. Every number below reproduces
with the commands shown; the synthetic input is deterministic (fixed seed),
so the workload is byte-identical across machines.

## Question

After the zero-copy refactors (market ids as views into the simdjson
buffer, allocation-free registry lookups), is the allocator still on the
replay hot path, and does running it on bdlma arenas buy anything?

## Setup

```
cmake -B build-bde -G Ninja -DCMAKE_BUILD_TYPE=Release -DBASIS_ENABLE_BDE=ON
cmake --build build-bde -j
./build-bde/src/basis synth /tmp/bench.feedlog --steps 200000 --lead-ms 400
```

That synthetic session is 724,298 records (124 MB) in real wire formats.
The live capture referenced below is a 60 s Polymarket market-channel
recording (877 records) used for allocation profiles only; it is too short
for stable timing percentiles.

Modes, all via the same binary:

```
./build-bde/src/basis replay /tmp/bench.feedlog --alloc heap
./build-bde/src/basis replay /tmp/bench.feedlog --alloc bde
./build-bde/src/basis replay /tmp/bench.feedlog --alloc count
```

heap is the default global allocator. bde puts parse transients on a
bdlma::SequentialAllocator released after every message and order book
nodes on a bdlma::MultipoolAllocator. count wraps the same seams in a
counting memory_resource.

## Results

Heap traffic per message (--alloc count):

| workload            | parse allocs/msg | parse bytes/msg | book allocs/msg |
| ------------------- | ---------------- | --------------- | --------------- |
| synthetic (724,298) | 1.0              | 56 B            | 0.5             |
| live capture (877)  | 2.0              | 574 B           | 0.9             |

The parse cost is the deltas vector and nothing else; the budget is pinned
by a unit test (CountingResource.ParseOfALargeBookStaysWithinBudget), so a
reintroduced per-delta copy fails CI rather than surfacing here.

Pipeline time, 8 interleaved runs per mode on the synthetic session
(pipeline = sum of per-record parse -> normalize -> analytics -> publish
spans, file io excluded):

| mode | best      | median    | best throughput   |
| ---- | --------- | --------- | ----------------- |
| heap | 195.5 ms  | 199.2 ms  | 3.70M records/sec |
| bde  | 197.6 ms  | 202.1 ms  | 3.65M records/sec |

Run-to-run noise on a desktop OS is 2 to 20 percent, larger than the gap
between modes in every interleaved pair.

## Conclusion

Parity. The zero-copy refactors already removed allocation from the
critical path; one or two bump allocations per message are within malloc's
small-object fast path, and simdjson parsing dominates the profile. The
engine ships with the global heap as default and keeps the bdlma path
behind --alloc bde.

The pmr seams (injectable parse arena, injectable book resource) stay,
because they are what made this measurable at all, and because the
calculus changes where the same code runs hotter: many feed threads
contending on one heap, or books deep enough that node locality matters.
The claim this benchmark supports is deliberately narrow: the hot path is
allocator-clean, proven by measurement, not by choice of allocator.
