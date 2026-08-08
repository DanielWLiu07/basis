# Subscription fan-out with slow consumers

The market-data distribution problem: one event, many subscribers, and no
guarantee they all keep up. `docs/bench/matching_engine.md` covers the
venue side; this is the consumer side.

## The property that makes market data different

A slow consumer of a trade log must eventually see every fill. A slow
consumer of a price feed wants the CURRENT price, and every value it
missed is one it no longer cares about. That asymmetry is what conflation
exploits: `api::ConflatingSession` gives each subscriber one slot per
topic instead of a queue, so a second update to an undrained topic
overwrites the first.

Two properties follow:

- Publishers never wait on consumers. `publish()` writes into each
  interested subscriber's slot and returns; it never runs a handler.
- Memory is bounded by subscribers times topics, not by publish rate or by
  how far behind a consumer falls. A queue per subscriber would grow
  without limit under exactly the conditions where growing is least
  affordable.

The cost is stated rather than hidden: intermediate values are dropped for
a subscriber that cannot keep up. Correct for quotes, wrong for anything a
consumer must see every instance of. That consumer wants
`core::BoundedQueue`, which blocks rather than drops, and the engine's
live path uses it for exactly that reason.

## What the synchronous session does instead

`api::InProcessSession` invokes every handler inline on the publisher's
thread. It is the right default for a single in-process consumer and it is
what the replay path uses. Under fan-out it has one fatal property: the
slowest consumer sets the publisher's rate.

    ./build/src/basis fanout-bench --subscribers 64 --slow 1 \
        --updates 20000 --slow-us 50

One subscriber whose handler takes 50 microseconds, everyone else instant.
Both sessions carry the identical update stream:

    subscribers   sync publish    conflating publish   speedup
    16            19,591 /sec       899,193 /sec        45.9x
    64            16,409 /sec       109,208 /sec         6.7x
    256           19,705 /sec       116,136 /sec         5.9x

Read the sync column first: it is pinned near 20,000 updates/sec at every
fan-out size, which is exactly 1 / 50 microseconds. The publisher is not
slowed down by the number of subscribers, it is slowed down to the rate of
the one slow handler. That is the coupling, and it is scale-free: a single
consumer that takes a millisecond would cap the publisher at 1,000
updates/sec no matter how fast the rest of the system is.

The conflating column falls off above 16 subscribers because this machine
has 10 cores and the benchmark runs one drainer thread per subscriber, so
at 256 subscribers the box is oversubscribed 25x and the publisher is
competing with consumer threads for CPU. That is a property of the test
harness, not of the session; the 16-subscriber row is the one that shows
the delivery model's own cost.

## Joining a stream that is already running

A consumer that connects mid-session has a problem the fan-out itself does
not solve: until the next tick on each topic it knows nothing, and on a
quiet book that can be minutes of blank screen. The venue answer is
snapshot-then-stream, and the session does it on subscribe: the last
published value per topic is cached, and subscribing seeds the joiner's
slot with it, so its first drain delivers the present image.

The seam is the part worth getting right. Roster insertion, handler
registration and the snapshot all happen under the same lock publish()
holds while it fans out, so any concurrent publish is ordered wholly
before the join (its value is the snapshot) or wholly after (the roster
already lists the joiner, so the normal path delivers it). The joiner
cannot miss an update, and cannot be handed an image older than a value
already waiting in its slot.

Registering the handler outside that lock was a real defect, not a
hypothetical one: a publish landing in the gap would slot a value while a
concurrent drain found no handler for the topic, skipped it, and cleared
the pending set - losing the value until the next tick. Holding one lock
across the whole join removes the window.

The cache is one entry per topic, so it is bounded by the configured
market set rather than by subscriber count or publish rate. Publishing
interns a topic even when nobody is listening yet, because that is
precisely the value a later joiner will ask for.

A test pins the race directly: 16 subscribers join while a publisher runs
20,000 updates flat out, and every one of them must end holding the final
value whichever side of the fan-out its join landed on.

## Correctness under conflation

Every run reports `worst_staleness_updates`, the largest gap between the
final published value and the last value any subscriber actually saw. It
is 0 in every configuration above, including for the slow subscriber. The
slow consumer sees fewer values, and the values it sees are current. That
is the whole claim: conflation drops the stale middle, never the present.

Under the 64-subscriber run, roughly 1.24M slotted values were superseded
before delivery and about 40,000 were delivered. A high conflation count
is the mechanism working, not a loss.

Tests pin the semantics: latest-value-not-every-value, no conflation for a
consumer that keeps up, subscriber and topic isolation, bounded memory
(100,000 publishes to an undrained subscriber cost one slot), the publisher
not stalling behind a sleeping handler, and a concurrency test asserting
that every slotted value is either delivered or conflated with four
publishers and eight subscribers running at once.
