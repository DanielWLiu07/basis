# Things that broke, and how they were found

Four defects that survived their own test suites. They are collected here
because the interesting part of each is not the fix, which is usually a few
lines, but the reason nothing caught it: in every case the code did exactly
what it was written to do, reported success, and produced a plausible
number.

Every figure below is from the run that exposed the bug.

## 1. A book that filled with phantom levels, and a spread of minus $54.87

**Symptom that gave it away:** an implausible ratio, not a crash. Binance's
mid registered 23 moves against Coinbase's 296 over the same window, for
two venues quoting the same asset with an identical $0.01 median spread.
Thirteen to one is not a market-structure difference.

**Cause.** Binance's `bookTicker` stream publishes the current best bid and
ask. The parser turned that into two `Set` deltas, which is correct for a
level diff and wrong for a message that *replaces* the touch: the previous
best bid and ask stayed resting in the book. `best_bid()` therefore decayed
into the running maximum of every bid ever quoted, and `best_ask()` into
the running minimum. By message 20,000 the book held 204 phantom levels and
a spread of **minus $54.87**, and the mid it produced wandered over a $25
band while the real one moved $55.

**Why nothing caught it.** The only consumer of that stream was the ingest
benchmark, which measures parse-and-apply throughput. A wrong book costs
exactly nothing there - it is the same number of operations on the same
number of levels. The parser tests asserted the deltas it emitted, which
were the deltas it was written to emit.

**How it was confirmed.** An independent 20-line Python replay of the same
capture, reconstructing the book from the raw payloads. Reimplementing the
thing you doubt, in another language, is cheap next to arguing with your
own code.

**Fix.** Emit `Action::Clear` ahead of the touch. Cross-correlation between
the venues went from 0.175 to 0.471 once the book was real, and ingest
throughput went from 932k to 2.19M messages/sec as a side effect, because
the book had stopped accumulating garbage levels to walk.

**What it changed about how this repo works.** A parser test now asserts
the *book state* the deltas produce, not just the deltas. Throughput
benchmarks are not correctness tests and are no longer treated as evidence
of any.

## 2. A feed that went silent for 25 minutes and reported perfect health

**Symptom.** A 45 minute two-venue capture lost Coinbase 19 minutes in. It
stayed silent for the remaining 25 and the client logged **zero
reconnects**, while Binance recovered twice across the same outage. Both
went quiet at the same instant, so the cause was local. The recorder
printed a frozen message count next to a climbing one for 25 minutes and
exited reporting zero malformed.

**Cause.** Reconnection was driven entirely by read errors, and a network
that goes away without closing anything does not produce one. A half-open
TCP connection leaves a blocking read parked forever, so the recovery path
never ran. Binance's server closes connections on its own schedule, which
is the only reason it recovered.

**Why nothing caught it.** The reconnect test suite injects hard TCP
closes, which is a fault that produces an error. No test produced silence.
There was also a comment asserting that Beast's built-in keep-alive pings
covered this; they do not, twice over - `stream_base::timeout` applies to
asynchronous operations and this client reads synchronously, and
`keep_alive_pings` is inert while `idle_timeout` is `none`, the client-role
default.

**Why it matters more than it looks.** For a market-data engine this is the
worst failure mode available: **a dead feed and a quiet market produce the
same observation.** While reading that capture the low event counts were
first attributed to a quiet market, which is exactly the wrong conclusion
and exactly the one the instrument invited.

**Fix.** A watchdog thread that breaks the read when nothing has arrived
for an idle budget, using the same socket shutdown `stop()` uses so the run
loop reconnects instead of exiting. `stalls()` is reported separately from
`reconnects()` everywhere, because they mean different things: a stall is a
fault that reports itself as perfect health.

## 3. A handshake with no timeout, found by the test for the bug above

**Symptom.** The new watchdog test hung instead of failing. A stack sample
showed the main thread parked in `WsClient::stop()` inside
`std::thread::join()`, and the IO thread parked in the synchronous TLS
handshake.

**Cause.** The TLS and WebSocket handshakes are synchronous and have no
timeout, and the connection was only published for shutdown *after* they
succeeded. A peer that accepts the TCP connection and then says nothing -
the test server, once its script had finished and its acceptor was still
open - parked the IO thread forever, and took `stop()` down with it, since
`stop()` joins that thread.

**Why nothing caught it.** The header documented this as a limitation that
resolves once the attempt "returns or times out." Against a silent peer it
does neither. Every existing test used a server that either answered or
closed.

**Fix.** Publish the connection for shutdown as soon as the TCP connect
returns, so both `stop()` and the watchdog can break a wedged handshake.
`send()` still uses the fully established handle alone, because writing to
a stream mid-handshake would corrupt it.

**One thing built wrong on the way.** The first version charged handshakes
against the same idle budget as steady-state data. A short idle timeout
then killed every connection attempt before it could finish, and the client
livelocked while filling the log with successful-looking reconnects.
Connection establishment and data silence are different budgets; they are
now separate fields with the reason written next to them.

## 4. Two hundred and sixty-seven arbitrages that were floating-point noise

**Symptom.** The analytics reported 267 complementary YES/NO no-arbitrage
violations on a capture. That is a large number of free money.

**Cause.** The bound was evaluated in floating point on values that are
exact integers in cents. Prices that summed to exactly $1.00 compared as
$1.0000000000000002, and every one of them counted as a violation.

**Why nothing caught it.** The test fixtures used prices that happened to
sum cleanly in binary. The real answer, once the comparison ran on integer
cents, was approximately zero - which is the correct answer for a liquid
market and the boring one, so a wrong result here looked like a finding
rather than a bug.

**Fix.** The bound is evaluated on integer cents. The general rule now
followed in this repo: prices are integers, and any comparison that decides
whether money is on the table runs on the integer representation.

## The pattern

None of these produced an error. Each one produced a plausible number, and
in three of the four cases the plausible number was *more* interesting than
the truth - a 13:1 move ratio, a quiet market, 267 arbitrages. That is the
direction the errors ran, and it is why this repo leans on cross-checks
that share no code with the thing being checked: an independent Python
replay, a matching engine that recomputes the analytics by executing the
same sweeps as order flow, a book rebuilt from diffs and compared against
the venue's own snapshot.
