#!/usr/bin/env python3
"""Does a short capture predict a long one?

The tempting shortcut with a benchmark is to run it briefly and scale the
result. This asks whether that is legitimate, on data where the answer is
checkable: the committed four-hour soak. Slice it into windows, replay
each one through the real pipeline, and compare every window's estimate
against the whole capture's measured value.

A statistic that survives is one you may quote from a short run. One that
does not is one you may only quote from a run at least as long as the
thing you are claiming, and saying which is which is the point.

    scripts/extrapolate.py <capture.feedlog> [--window-min 30]

Result on the committed four-hour soak, written up in
docs/bench/extrapolation.md: the median extrapolates from a ten-minute
window within 5.3%, and the 99th percentile does not - one window reported
1.21 us against a true 76.46, off by a factor of 63 in the direction that
flatters. The rule that follows is that a short run may be quoted for the
median and for ratios of counts, and for nothing else.
"""

import argparse
import json
import pathlib
import statistics
import subprocess
import sys
import tempfile

BIN = "./build/src/basis"
CONFIG = "configs/contracts.toml"


def replay(path):
    out = subprocess.run([BIN, "replay", path, "--config", CONFIG, "--json"],
                         capture_output=True, text=True, timeout=900).stdout
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def stats_of(d):
    """The figures a reader might be tempted to quote from a short run."""
    if not d:
        return None
    lat = d.get("latency_us", {})
    pipe = d.get("pipeline", {})
    records = d.get("records", 0) or 1
    return {
        "records/sec (venue)": d.get("session", {}).get("records_per_sec")
        or (records / max(d.get("session", {}).get("span_seconds", 1), 1)),
        "deltas/record": d.get("deltas", 0) / records,
        "throughput k/sec": pipe.get("records_per_sec", 0) / 1000.0,
        "latency p50 us": lat.get("p50", 0.0),
        "latency p99 us": lat.get("p99", 0.0),
        "latency max us": lat.get("max", 0.0),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("--window-min", type=float, default=30.0)
    args = ap.parse_args()

    lines = pathlib.Path(args.capture).read_text(errors="replace").splitlines()
    stamped = []
    for line in lines:
        tab = line.find("\t")
        if tab <= 0:
            continue
        try:
            stamped.append((int(line[:tab]), line))
        except ValueError:
            continue
    if not stamped:
        print("no usable records")
        return 1

    t0 = stamped[0][0]
    span_s = (stamped[-1][0] - t0) / 1e9
    width_ns = int(args.window_min * 60 * 1e9)
    n_windows = max(1, int(span_s * 1e9 // width_ns))
    print(f"{len(stamped)} records over {span_s / 3600:.2f} h, "
          f"{n_windows} windows of {args.window_min:.0f} min\n")

    whole = stats_of(replay(args.capture))
    if whole is None:
        print("could not replay the whole capture")
        return 1

    per_window = []
    for w in range(n_windows):
        lo, hi = t0 + w * width_ns, t0 + (w + 1) * width_ns
        chunk = [ln for ts, ln in stamped if lo <= ts < hi]
        if len(chunk) < 50:
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".feedlog",
                                         delete=False) as f:
            f.write("\n".join(chunk) + "\n")
            tmp = f.name
        s = stats_of(replay(tmp))
        pathlib.Path(tmp).unlink()
        if s:
            per_window.append(s)

    if len(per_window) < 2:
        print("not enough windows to compare")
        return 1

    print(f"{'statistic':22s} {'whole':>12s} {'window min':>12s} "
          f"{'window max':>12s} {'worst error':>12s}  verdict")
    print("-" * 88)
    for key in whole:
        truth = whole[key]
        vals = [w[key] for w in per_window]
        lo, hi = min(vals), max(vals)
        if truth == 0:
            continue
        worst = max(abs(v - truth) / abs(truth) for v in vals) * 100.0
        # A statistic a short run may be trusted for is one whose worst
        # window is close to the whole. 25% is a generous bar and the
        # separation here is not subtle.
        verdict = "extrapolates" if worst < 25 else "DOES NOT extrapolate"
        print(f"{key:22s} {truth:12.2f} {lo:12.2f} {hi:12.2f} "
              f"{worst:11.1f}%  {verdict}")

    print(f"\n{len(per_window)} windows compared against the whole capture.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
