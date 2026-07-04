#!/usr/bin/env python3
"""Renders the README figures from feedlog captures.

Two subcommands, one per figure:

  live   mid-price series for the most active events in a live capture
         (docs/bench/live-poly-30min.feedlog.gz is the committed input)
  synth  the closed-loop validation picture: synthetic Kalshi and
         Polymarket mids with the injected lead visible

The script rebuilds top-of-book from the raw wire messages independently
of the C++ engine, so the figures double as a cross-check of the parsers:
if the pictures disagreed with replay's numbers, one of the two book
implementations would be wrong.

Only needs matplotlib. Usage:

  python3 scripts/plot_bench.py live docs/bench/live-poly-30min.feedlog.gz \\
      --config configs/contracts.toml --out docs/img/live-mids.png
  python3 scripts/plot_bench.py synth captures/demo.feedlog \\
      --out docs/img/synth-lead.png
"""

import argparse
import gzip
import json
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e6e5e1"
SERIES = ["#2a78d6", "#1baf7a", "#eda100", "#008300"]


def open_feedlog(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "rt", encoding="utf-8", errors="replace")


def read_records(path):
    """Yields (recv_ns, venue, payload) from a feedlog."""
    with open_feedlog(path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t", 2)
            if len(parts) != 3:
                continue
            try:
                yield int(parts[0]), parts[1], parts[2]
            except ValueError:
                continue


def load_yes_tokens(config_path):
    """Maps YES token id -> event id from the contracts registry."""
    tokens = {}
    event_id = None
    with open(config_path) as f:
        for line in f:
            m = re.match(r'^id = "(.+)"', line)
            if m:
                event_id = m.group(1)
            m = re.match(r'^polymarket_token = "(.+)"', line)
            if m and event_id:
                tokens[m.group(1)] = event_id
    return tokens


class Book:
    """Minimal top-of-book: enough to compute a mid, nothing more."""

    def __init__(self):
        self.bids = {}
        self.asks = {}

    def mid(self):
        if not self.bids or not self.asks:
            return None
        return (max(self.bids) + min(self.asks)) / 2.0


def apply_polymarket(event, books, on_mid, recv_ns):
    kind = event.get("event_type")
    if kind == "book":
        book = books.setdefault(event.get("asset_id"), Book())
        book.bids.clear()
        book.asks.clear()
        for side_key, side in (("bids", book.bids), ("asks", book.asks)):
            for level in event.get(side_key, []):
                price = round(float(level["price"]) * 100)
                size = float(level["size"])
                if size > 0:
                    side[price] = size
        on_mid(event.get("asset_id"), book.mid(), recv_ns)
    elif kind == "price_change":
        for change in event.get("price_changes", []):
            book = books.setdefault(change.get("asset_id"), Book())
            price = round(float(change["price"]) * 100)
            size = float(change["size"])
            side = book.bids if change.get("side") == "BUY" else book.asks
            if size <= 0:
                side.pop(price, None)
            else:
                side[price] = size
            on_mid(change.get("asset_id"), book.mid(), recv_ns)


def collect_polymarket_mids(path, wanted_tokens):
    """Returns {token: [(t_ns, mid_cents), ...]} for the wanted tokens."""
    books = {}
    series = {t: [] for t in wanted_tokens}

    def on_mid(token, mid, recv_ns):
        if token in series and mid is not None:
            series[token].append((recv_ns, mid))

    for recv_ns, venue, payload in read_records(path):
        if venue != "polymarket":
            continue
        try:
            doc = json.loads(payload)
        except json.JSONDecodeError:
            continue
        events = doc if isinstance(doc, list) else [doc]
        for event in events:
            if isinstance(event, dict):
                apply_polymarket(event, books, on_mid, recv_ns)
    return series


def collect_kalshi_mids(path):
    """Returns [(t_ns, mid_cents), ...] for the single synthetic market."""
    book = Book()
    out = []
    for recv_ns, venue, payload in read_records(path):
        if venue != "kalshi":
            continue
        try:
            doc = json.loads(payload)
        except json.JSONDecodeError:
            continue
        msg = doc.get("msg", {})
        kind = doc.get("type")
        if kind == "orderbook_snapshot":
            book.bids.clear()
            book.asks.clear()
            for price, size in msg.get("yes", []):
                book.bids[price] = size
            for price, size in msg.get("no", []):
                book.asks[100 - price] = size  # NO bid at p = YES ask at 100-p
        elif kind == "orderbook_delta":
            price = msg.get("price")
            side = book.bids if msg.get("side") == "yes" else book.asks
            key = price if msg.get("side") == "yes" else 100 - price
            size = side.get(key, 0) + msg.get("delta", 0)
            if size <= 0:
                side.pop(key, None)
            else:
                side[key] = size
        else:
            continue
        mid = book.mid()
        if mid is not None:
            out.append((recv_ns, mid))
    return out


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(GRID)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9)


def plot_live(args):
    tokens = load_yes_tokens(args.config)
    series = collect_polymarket_mids(args.feedlog, set(tokens))
    ranked = sorted(series.items(), key=lambda kv: len(kv[1]), reverse=True)
    top = [(tokens[tok], pts) for tok, pts in ranked[:3] if len(pts) > 1]
    if not top:
        sys.exit("no plottable series in " + args.feedlog)

    t0 = min(pts[0][0] for _, pts in top)
    fig, ax = plt.subplots(figsize=(9.0, 3.6), dpi=160)
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)

    for i, (event_id, pts) in enumerate(top):
        minutes = [(t - t0) / 60e9 for t, _ in pts]
        mids = [m for _, m in pts]
        ax.plot(minutes, mids, drawstyle="steps-post", linewidth=2.0,
                color=SERIES[i], label=event_id, solid_capstyle="round")
        ax.annotate(event_id, xy=(minutes[-1], mids[-1]),
                    xytext=(6, 0), textcoords="offset points",
                    va="center", fontsize=9, color=TEXT_SECONDARY)

    ax.set_xlabel("minutes into capture", color=TEXT_SECONDARY, fontsize=9)
    ax.set_ylabel("mid price (cents = implied %)",
                  color=TEXT_SECONDARY, fontsize=9)
    ax.set_title("Normalized Polymarket mids, 30 minute live capture",
                 color=TEXT_PRIMARY, fontsize=11, loc="left", pad=10)
    ax.legend(loc="center left", frameon=False, fontsize=9,
              labelcolor=TEXT_SECONDARY)
    ax.margins(x=0.10)
    fig.tight_layout()
    fig.savefig(args.out, facecolor=SURFACE, bbox_inches="tight")
    print("wrote", args.out)


def plot_synth(args):
    kalshi = collect_kalshi_mids(args.feedlog)
    poly_series = collect_polymarket_mids(args.feedlog, {args.token})
    poly = poly_series[args.token]
    if not kalshi or not poly:
        sys.exit("no plottable series in " + args.feedlog)

    # A short window keeps individual quote steps, and therefore the lag
    # between them, visible to the eye.
    t0 = kalshi[0][0]
    lo, hi = args.window_start, args.window_start + args.window_seconds

    def window(points):
        return [((t - t0) / 1e9, m) for t, m in points
                if lo <= (t - t0) / 1e9 <= hi]

    kalshi_w, poly_w = window(kalshi), window(poly)

    fig, ax = plt.subplots(figsize=(9.0, 3.6), dpi=160)
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)

    for pts, color, label in ((kalshi_w, SERIES[0], "kalshi mid"),
                              (poly_w, SERIES[1], "polymarket mid")):
        ax.plot([t for t, _ in pts], [m for _, m in pts],
                drawstyle="steps-post", linewidth=2.0, color=color,
                label=label, solid_capstyle="round")

    # Point at the lag once, on the window's largest single move: the same
    # step appears on the lagging series exactly lead seconds later.
    biggest = max(range(1, len(kalshi_w)),
                  key=lambda i: abs(kalshi_w[i][1] - kalshi_w[i - 1][1]))
    step_t = kalshi_w[biggest][0]
    step_mid = (kalshi_w[biggest][1] + kalshi_w[biggest - 1][1]) / 2.0
    ax.annotate("", xy=(step_t + args.lead_seconds, step_mid),
                xytext=(step_t, step_mid),
                arrowprops=dict(arrowstyle="<->", color=TEXT_SECONDARY,
                                linewidth=1.2))
    ax.annotate(f"{args.lead_seconds * 1000:.0f} ms",
                xy=(step_t + args.lead_seconds / 2.0, step_mid),
                xytext=(0, 7), textcoords="offset points",
                ha="center", fontsize=9, color=TEXT_SECONDARY)

    ax.set_xlabel("seconds into synthetic session",
                  color=TEXT_SECONDARY, fontsize=9)
    ax.set_ylabel("mid price (cents)", color=TEXT_SECONDARY, fontsize=9)
    ax.set_title("Closed-loop validation: injected 400 ms lead, "
                 "recovered by replay",
                 color=TEXT_PRIMARY, fontsize=11, loc="left", pad=10)
    ax.legend(loc="lower left", frameon=False, fontsize=9,
              labelcolor=TEXT_SECONDARY)
    fig.tight_layout()
    fig.savefig(args.out, facecolor=SURFACE, bbox_inches="tight")
    print("wrote", args.out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    live = sub.add_parser("live", help="mid series from a live capture")
    live.add_argument("feedlog")
    live.add_argument("--config", default="configs/contracts.toml")
    live.add_argument("--out", default="docs/img/live-mids.png")
    live.set_defaults(func=plot_live)

    synth = sub.add_parser("synth", help="closed-loop lead figure")
    synth.add_argument("feedlog")
    synth.add_argument("--token", default="1000001")
    synth.add_argument("--lead-seconds", type=float, default=0.4)
    synth.add_argument("--window-start", type=float, default=60.0)
    synth.add_argument("--window-seconds", type=float, default=6.0)
    synth.add_argument("--out", default="docs/img/synth-lead.png")
    synth.set_defaults(func=plot_synth)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
