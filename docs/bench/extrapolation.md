# Can a short capture stand in for a long one?

The tempting shortcut with any benchmark is to run it briefly and scale
the result. This measures whether that is legitimate, on data where the
answer is checkable.

    ./scripts/extrapolate.py docs/bench/soak.feedlog --window-min 10

The committed four-hour soak is sliced into windows, each replayed through
the real pipeline, and every window's estimate compared against the whole
capture's measured value. A statistic whose worst window lands near the
truth is one a short run may be quoted for. One whose worst window does
not is a statistic that requires a run at least as long as the claim.

## The result

Ten-minute windows, nine of them carrying enough traffic to measure,
against a 3.56 hour capture:

| statistic | whole capture | window min | window max | worst error |
|:--|--:|--:|--:|--:|
| latency p50 | 0.79 us | 0.75 | 0.83 | **5.3%** |
| deltas per record | 8.26 | 4.95 | 13.20 | 59.9% |
| pipeline throughput | 452 k/sec | 316 | 671 | 48.5% |
| latency max | 1,189 us | 104 | 868 | 91.3% |
| latency p99 | 76.46 us | **1.21** | 86.12 | **98.4%** |

**The median extrapolates and the tail does not.** A ten-minute window
predicts the four-hour p50 within 5%. The same window length produced a
p99 of 1.21 microseconds against a true 76.46 - off by a factor of 63,
in the direction that flatters.

## Why the tail behaves that way

The p50 describes what the engine does on a typical message, and typical
messages are the overwhelming majority in any window. The p99 describes
what it does on the rare ones, and in this feed the rare ones are full
book snapshots: a 1.13 MB Coinbase image is four orders of magnitude
larger than a delta and takes correspondingly longer to parse.

A ten-minute window may contain no snapshot at all. When it does not, the
p99 it reports is the p99 of ordinary deltas, which is a real number
measuring the wrong population. Nothing about the output says which case
you got.

This is the same shape as the coordinated omission result in
`latency.md`, and worth seeing as one family: **a measurement that skips
the expensive events reports the cheap ones and calls the result a
percentile.** There, the expensive event was waiting for a turn. Here, it
is a snapshot arriving.

## The rate figure, and what it says about the session

The venue's average rate over the whole capture is 4.67 messages/sec. Over
the windows that carried traffic it is 12 to 18. That gap is not noise:
twelve of the twenty-one windows held fewer than fifty records, because
the session includes three real disconnects and the reconnect gaps around
them.

So "the venue produces 4.67 messages/sec" and "the venue produces 15
messages/sec" are both true and describe different things - one includes
the time it was not producing anything. A capture used to size a consumer
wants the first; one used to size a parser wants the second.

## What this repo may therefore claim from a short run

  the median ingest-to-signal latency, and only that;
  anything that is a ratio of counts, since those do not depend on how
  long you watched.

And what it may not:

  any percentile above the median, any maximum, and any throughput,
  without a run at least as long as the period being described.

That rule is why `latency.md`'s headline figures come from the committed
thirty-minute and four-hour captures rather than from a convenient slice,
and why this file exists rather than a sentence asserting that short runs
are representative.
