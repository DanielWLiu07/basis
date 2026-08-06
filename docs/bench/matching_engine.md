# Matching engine: design, correctness, throughput

`src/exec/limit_order_book.h` is a price-time-priority limit order book
with matching. Everything else in basis consumes venue data; this produces
it, which is what lets the executable-edge numbers be checked against
simulated fills instead of trusted as formulas.

## Why a flat array instead of a tree

Prediction-market contracts settle at $0 or $1, so a resting price is an
integer cent strictly inside that range: 1..99, which is also the implied
probability. A general equity book needs a map or tree keyed by an
unbounded price. Here the entire ladder is 99 slots, so:

- price lookup is a direct array index, no comparisons and no pointer
  chasing;
- best-price maintenance is a bit scan over two 64-bit occupancy words
  (`std::countl_zero` / `std::countr_zero`), not a tree walk;
- levels never allocate: they are fixed slots that hold an intrusive FIFO
  head and tail.

Orders live in a slab with a free list, so steady-state add/cancel churn
allocates nothing (cancelled slots are recycled). Each price level is an
intrusive doubly-linked FIFO of slab indices, which makes time priority
exact and cancellation position-independent.

Complexity, all independent of book depth: submit is O(1) per price level
touched and O(1) when it rests without crossing, cancel is O(1) (id to
slab index, then an unlink), best bid/ask is O(1).

Order types: Gtc (rest the remainder), Ioc (cancel the remainder), Fok
(all-or-nothing, decided by a non-mutating pre-check so a rejected order
leaves the book byte-identical). Fills report the resting order's price,
which is the price-time-priority rule: the order that was there first sets
the terms.

## Correctness

Two independent checks, both in CI:

1. **Differential against a reference book.** The benchmark carries a
   textbook `std::map<price, list<order>>` implementation with the same
   semantics and replays the identical order flow through both. Over
   200,000 random operations they must produce the same fill count, the
   same traded size, and the same notional, or the test fails. That is
   what makes the timing comparison meaningful: same workload, two
   layouts.
2. **Against the analytics.** `model::crossed_sweep_cents` computes what
   sweeping a crossed book pays by walking the two books arithmetically.
   The matching engine gets the same answer by executing the sweep as
   order flow: rest the cheap venue's asks, hit them with one Ioc buy per
   rich bid, best bid first, and sum (bid - fill price) x size. Both
   implementations must agree on every fixture, which turns the reported
   edge numbers into a checked claim rather than a trusted formula.

Plus the unit suite: FIFO time priority within a price, maker price sets
the trade price, multi-level walks, remainder resting, Ioc and Fok
semantics, price-bound and duplicate-id rejection, best-price maintenance
across the occupancy word boundary (prices 63 and 64), and a 500-order
churn test that reconciles per-level sizes against the side total.

## Throughput

    ./build/src/basis lob-bench --ops 2000000

Order flow shaped like a quoting venue: 60% passive quotes 1 to 5 cents
behind a random-walking mid, 30% cancels of earlier orders, 10% aggressive
orders priced through the mid. Deterministic (fixed seed), generated once,
replayed by both implementations, so generation cost is never inside a
timed region.

Apple M4, Release, 2,000,000 operations (1.17M of them producing fills,
65.6M contracts traded):

    flat ladder     34.6 ns/op     28.9M ops/sec
    std::map book   45.7 ns/op     21.9M ops/sec
    ratio           1.33x

Stable across runs at 1.25 to 1.33x. The ratio is modest by design: both
books share the same order-id hash map and the same matching loop, so the
ladder's advantage is confined to price lookup and best-price
maintenance. The absolute number is the headline (an engine that keeps up
with any prediction-market venue's message rate by three orders of
magnitude); the ratio is the evidence that the bounded-price insight is
worth the specialized layout.

`scripts/perf_gate.sh` runs the benchmark on every commit and fails the
build if the two books disagree on any fill or if throughput falls an
order of magnitude below developer hardware.
