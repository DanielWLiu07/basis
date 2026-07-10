#!/usr/bin/env bash
# Performance regression gate, run by CI on every commit.
#
# Generates the deterministic synthetic session (fixed seed, so the input
# is byte-identical everywhere) and replays it through the full pipeline,
# then asserts the properties that must not regress:
#
#   1. Correctness of the closed loop: the injected 400 ms cross-venue
#      lead is recovered, with high correlation.
#   2. Integrity accounting: zero malformed messages, zero broken lines,
#      zero gaps. The synthetic input is clean; anything else is a parser
#      regression.
#   3. Allocation budget: the hot path stays at vector-growth-only
#      allocation per message (see docs/bench/allocator.md).
#   4. A throughput floor with roughly 10x headroom below developer
#      hardware, so shared CI runners never flake on noise. This catches
#      an accidental O(n) walk per message, not a few percent.
#
# Tunable via environment for local runs; defaults are the CI contract.

set -euo pipefail

BIN=${1:-build/src/basis}
STEPS=${PERF_GATE_STEPS:-50000}
MIN_KRPS=${PERF_GATE_MIN_KRPS:-300}
MAX_PARSE_PER_MSG=${PERF_GATE_MAX_PARSE_PER_MSG:-1.2}
MAX_BOOKS_PER_MSG=${PERF_GATE_MAX_BOOKS_PER_MSG:-0.7}

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

"$BIN" synth "$workdir/gate.feedlog" --steps "$STEPS" --lead-ms 400 \
    > /dev/null
report=$("$BIN" replay "$workdir/gate.feedlog" \
    --config configs/synthetic.toml --alloc count --json)
printf '%s\n\n' "$report"

# Pull the gated fields out of the structured output, so a change to the
# human report format cannot silently break the gate. python3 is present
# on every CI runner; no jq dependency.
read -r lead corr krps parse_per_msg books_per_msg malformed bad_lines gaps \
    <<EOF
$(printf '%s' "$report" | python3 -c '
import json, sys
d = json.load(sys.stdin)
e = d["events"][0]["lead_lag"]
print(e["lead_seconds"], e["correlation"],
      d["pipeline"]["records_per_sec"] / 1000.0,
      d["alloc"]["parse_per_msg"], d["alloc"]["book_per_msg"],
      d["malformed"], d["malformed_lines"], d["gaps"])
')
EOF

failures=0
check() {
  local name=$1 value=$2 expr=$3
  if [ -z "$value" ]; then
    echo "GATE FAIL  $name: value missing from replay output"
    failures=$((failures + 1))
  elif ! awk -v v="$value" "BEGIN { exit !($expr) }"; then
    echo "GATE FAIL  $name: $value violates $expr"
    failures=$((failures + 1))
  else
    echo "gate ok    $name = $value  ($expr)"
  fi
}

check "recovered lead (s)"   "$lead"          "v >= 0.395 && v <= 0.405"
check "lead correlation"     "$corr"          "v >= 0.99"
check "malformed messages"   "$malformed"     "v == 0"
check "broken feedlog lines" "$bad_lines"     "v == 0"
check "sequence gaps"        "$gaps"          "v == 0"
check "parse allocs per msg" "$parse_per_msg" "v <= $MAX_PARSE_PER_MSG"
check "book allocs per msg"  "$books_per_msg" "v <= $MAX_BOOKS_PER_MSG"
check "throughput (k rec/s)" "$krps"          "v >= $MIN_KRPS"

if [ "$failures" -gt 0 ]; then
  echo "perf gate: $failures check(s) failed"
  exit 1
fi
echo "perf gate: all checks passed"
