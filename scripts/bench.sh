#!/usr/bin/env bash
# Regenerates the headline numbers in docs/bench from the committed
# artifacts, so every figure in the docs and README traces to one command
# rather than a remembered manual run. Reads replay --json, so a change to
# the human report format cannot drift the summary. python3 only, no jq.
#
#   ./scripts/bench.sh [path/to/basis]

set -euo pipefail

BIN=${1:-build/src/basis}
if [ ! -x "$BIN" ]; then
  echo "build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# Reads one replay json object on stdin and prints a one-line summary.
# The parser lives in a temp file so stdin stays the piped json rather than
# a script heredoc. .format keeps backslashes out of f-string expressions,
# which python rejects before 3.12.
cat > "$workdir/summarize.py" <<'PY'
import json, sys
label = sys.argv[1]
d = json.load(sys.stdin)
lat = d["latency_us"]
loss = d["malformed"] + d["malformed_lines"] + d["gaps"]
print("{:<14} {:>8} msgs  {:>9} deltas  p50 {:.1f}us  p99 {:.1f}us  "
      "{:.0f}k rec/s  hashes {}ok/{}bad  loss {}".format(
          label, d["records"], d["deltas"], lat["p50"], lat["p99"],
          d["pipeline"]["records_per_sec"] / 1000.0,
          d["hashes"]["verified"], d["hashes"]["mismatched"], loss))
PY
summarize() { python3 "$workdir/summarize.py" "$1"; }

echo "basis benchmark summary (docs/bench sources)"
echo

# Committed live captures: replay each through the full pipeline.
for gz in docs/bench/live-poly-30min.feedlog.gz docs/bench/soak.feedlog.gz; do
  [ -f "$gz" ] || continue
  name=$(basename "$gz" .feedlog.gz)
  feed="$workdir/$name.feedlog"
  gunzip -c "$gz" > "$feed"
  "$BIN" replay "$feed" --config configs/contracts.toml --json \
    | summarize "$name"
done

# Synthetic session: the closed loop plus the allocation budget and
# throughput on a clean, deterministic input.
"$BIN" synth "$workdir/synth.feedlog" --steps 200000 --lead-ms 400 > /dev/null
"$BIN" replay "$workdir/synth.feedlog" --config configs/synthetic.toml \
  --alloc count --json > "$workdir/synth.json"
summarize "synthetic" < "$workdir/synth.json"

python3 - "$workdir/synth.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
e = d["events"][0]["lead_lag"]
a = d["alloc"]
print()
print("  closed loop : lead {:.3f}s  95% ci {:.3f}..{:.3f}s  corr {:.2f}".format(
    e["lead_seconds"], e["ci_low_seconds"], e["ci_high_seconds"],
    e["correlation"]))
print("  hot path    : {:.1f} parse + {:.1f} book allocs/msg, {:.0f} B/msg".format(
    a["parse_per_msg"], a["book_per_msg"], a["parse_bytes_per_msg"]))
PY
