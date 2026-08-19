# Cross-venue lead: Binance against Coinbase

Every lead-lag number in this repo before now came from a synthetic
session with a lead injected on purpose, which proves the estimator
recovers what was put in and says nothing about a real market. This is
the first measurement of two live venues quoting the same instrument.

The pairing exists because it is the only one available without
credentials. Kalshi and Polymarket list matching contracts but the Kalshi
side needs an API key; Binance alone has nothing to be compared against.
Binance BTCUSDT and Coinbase BTC-USD are the same asset on two venues,
both public.

## How the capture is taken

    basis record out.feedlog --binance btcusdt --coinbase BTC-USD --seconds 2700

One process opens both sockets and stamps each message the instant it
returns from the socket. That single detail is what makes the measurement
possible at all: two processes, or two clocks, would put an unknown
offset directly into the quantity being estimated.

The engine captures its own data. `feed_live::BinanceFeed` and
`feed_live::CoinbaseFeed` run on the same Boost.Beast TLS WebSocket
client that carries Kalshi and Polymarket, so the capture path is the
code under test rather than a separate script that can drift from it.
The committed artifact below predates those adapters and was taken with
`scripts/capture_xvenue.py`, which remains for reference; the two write
the same format and the recorder is now the documented path.

- Binance: `btcusdt@bookTicker`, which pushes on every change of either
  side of the touch.
- Coinbase: `BTC-USD` on `level2_batch`, a full book image followed by
  diffs. The `level2` channel now requires authentication;
  `level2_batch` does not, and coalesces updates on a 50 ms timer.

Both venues land in one `.feedlog`, replayed by
`basis xvenue-lead <capture.feedlog>`.

## What the measurement found first: a broken book

The first run of this comparison produced a confident result built on a
corrupt book, and the way it surfaced is worth recording.

Binance's `bookTicker` publishes the current best bid and ask. The parser
turned that into two `Set` deltas, which is correct for a level diff and
wrong for a message that *replaces* the touch: the previous best bid and
ask stayed resting in the book. `best_bid()` therefore decayed into the
running maximum of every bid ever quoted and `best_ask()` into the
running minimum. On a BTCUSDT capture the book reached 204
phantom levels and a spread of **minus $54.87** by message 20,000, and
the mid it produced wandered over a $25 band while the real one moved
$55.

Nothing had caught it because the only consumer of that stream was the
ingest benchmark, which measures parse-and-apply throughput. A wrong book
costs exactly nothing there.

The symptom that gave it away was not a crash but an implausible ratio:
Binance's mid registered 23 moves against Coinbase's 296 over the same
window,
for two venues quoting the same asset with an identical $0.01 median
spread. Thirteen to one is not a market structure difference. The fix is
a `Clear` ahead of the touch, and cross-correlation between the venues
went from 0.175 to 0.471 once the book was real.

## Two estimators, because they fail differently

**Cross-correlation** (`CrossCorrelationEstimator`) resamples both mid
series onto a fixed grid and correlates returns across lags. It cannot
resolve a lead shorter than one bin, and the finest defensible bin here
is 50 ms, set by Coinbase's batch timer. On this capture it peaks at
+100 ms on the Binance side with a bootstrap interval of 0 to 100 ms.
An interval touching zero is not a resolved lead, so this method reports
the direction and declines to give a magnitude, which is a statement
about the instrument as much as the market.

**The event study** (`EventStudyEstimator`) does not grid. It isolates
discrete repricings on one venue and measures whether the other moves the
same way inside a window, then compares the forward and reverse follow
rates with a two-proportion z. It answers a question the grid cannot:
not *how far apart* the series are, but *whose moves get answered*.

The threshold has to be scaled to the instrument. The estimator's default
repricing is 1 cent, which is right for a contract trading between 1c and
99c and pure noise on one quoting around $63,000.

## Result

`docs/bench/btc-xvenue.feedlog.gz` is the capture these come from: 45
minutes, 204,864 messages (161,376 Binance, 43,487 Coinbase), zero
malformed, 5 levels dropped as unrepresentable. Sampling on a 50 ms
common clock, 2 s follow window:

| Move threshold | Binance moves | answered | Coinbase moves | answered | z | leader |
| ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| $0.25 | 471 | 272 (0.577) | 1039 | 274 (0.264) | 11.76 | Binance |
| $0.50 | 418 | 234 (0.560) | 869 | 230 (0.265) | 10.33 | Binance |
| $1.00 | 351 | 183 (0.521) | 605 | 174 (0.288) | 7.20 | Binance |
| $2.00 | 259 | 133 (0.514) | 392 | 104 (0.265) | 6.44 | Binance |

**A repricing on Binance is answered by Coinbase 57.7% of the time; one
on Coinbase is answered by Binance 26.4% of the time.** The gap holds at
every threshold tested, from a quarter-dollar bar up to two dollars, and
weakens only as the event count falls. That is what a real effect looks
like; an artifact of one threshold choice would not survive the sweep.

The cross-correlation estimator, on the same data, peaks at +100 ms with
the bootstrap interval running 0 to 100 ms. The peak is on the Binance
side, but an interval touching zero is not a resolved lead, and it is
reported as unresolved.

## Replication

One capture is an anecdote. The measurement above was repeated on a second
45 minute session taken eight days later
(`docs/bench/btc-xvenue-2.feedlog.gz`, 642,919 messages against the
first capture's 204,864 - the same window length in a market roughly three
times as busy). Both feeds ran continuously: no gap over five seconds on
either side, which is the first long capture taken since the idle watchdog
landed and the reason it is worth saying.

| Move threshold | Binance moves | answered | Coinbase moves | answered | z | leader |
| ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| $0.25 | 3720 | 3359 (0.903) | 9834 | 5304 (0.539) | 39.33 | Binance |
| $0.50 | 3541 | 3171 (0.896) | 8644 | 4737 (0.548) | 36.49 | Binance |
| $1.00 | 3260 | 2874 (0.882) | 6681 | 3816 (0.571) | 30.97 | Binance |
| $2.00 | 2764 | 2378 (0.860) | 4543 | 2681 (0.590) | 24.27 | Binance |

The direction replicates at every threshold, and the sample is large
enough that z runs from 24 to 39 rather than 6 to 12.

**What does not replicate is the follow rates themselves, and that is the
more useful thing to know.** Binance's rate went from 0.577 to 0.903 and
Coinbase's from 0.264 to 0.539: both roughly doubled. A two second window
catches more coincidence when a market is moving constantly, so an
absolute follow rate is a property of the session as much as of the venue
pair. The asymmetry survives - Binance is answered 1.7 times as often as
it answers here, against 2.2 times in the quieter session - but the ratio
is smaller on the busy day, in the direction more background movement
would push it.

So the claim these two captures jointly support is the ordering and its
significance, not "57.7% versus 26.4%". Those two numbers describe one
afternoon. Quoting them as a property of the venues would be the same
mistake as quoting a frame rate without naming the GPU.

## What would have to be wrong for this to be an artifact

Three things push the estimate, and only one pushes it toward the
conclusion.

**Network path, roughly 86 ms against the finding.** Median TCP connect
from the capture host is 189.6 ms to Binance and 18.8 ms to Coinbase
(seven attempts each; connect RTT is a proxy for path latency, not the
data path itself, and TLS adds round trips on top). Halving those for
one-way, Binance messages are stamped about 86 ms *later* than they
occurred relative to Coinbase.

That handicap works on both halves of the comparison at once. A genuine
Binance move answered by Coinbase in under 86 ms is observed with the
response arriving *before* the move, so it does not count as a follow at
all. In the reverse direction the same offset pushes Binance's answers
later, which keeps them comfortably inside the 2 s window and inflates
Coinbase's follow rate. The instrument suppresses the forward rate and
inflates the reverse one, which is precisely the shape of the result it
nonetheless reports.

**Coinbase's 50 ms batch timer, up to 50 ms toward the finding.**
`level2_batch` coalesces updates, delaying Coinbase's side by 0 to 50 ms
and making it look like the follower for a reason that is not price
discovery.

Netting those two, the instrument is biased *against* finding a Binance
lead by roughly 36 to 86 ms, and finds one anyway.

**Thin-book churn, direction uncertain, the weakest link.** Coinbase's
top of book is less deep than Binance's, so its mid moves more often;
some of those moves are quote churn rather than repricings. Because the z
compares follow *rates*, churn that nobody answers dilutes Coinbase's
rate and inflates the asymmetry. Raising the threshold is a partial
control (the effect survives it), but this is not fully separated and
should not be claimed as if it were.

## What this does and does not say

It says: on this capture, repricings on Binance are answered by Coinbase
far more often than the reverse, at a significance the sample supports,
under a measurement biased against that conclusion.

It does not say: that the lead is any particular number of milliseconds.
The cross-correlation's interval includes zero, and the event study's
median follow times land on 150 to 250 ms but are quantized to the 50 ms
sampling grid and sit on top of an 86 ms network offset that has not been
subtracted out. The tool reports them; they are not a latency figure and
should not be read as one. The defensible claim is the direction and its
significance, not a duration.

It is also not a causal claim, and not tradeable: a lead that survives an
86 ms network handicap from one laptop says nothing about what is
capturable by someone colocated.

## Reproducing

```
gunzip -c docs/bench/btc-xvenue.feedlog.gz > /tmp/btc-xvenue.feedlog
./build/src/basis xvenue-lead /tmp/btc-xvenue.feedlog --move-cents 25
./scripts/xvenue_report.sh /tmp/btc-xvenue.feedlog   # the sweep table above
```

Capture your own with `basis record --binance ... --coinbase ...` (both
sockets, one process, one clock). Numbers will differ with the venue's activity and with your own
network distance to each venue, which is the point of the bias section
above.
