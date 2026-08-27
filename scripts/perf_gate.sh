#!/usr/bin/env bash
# Performance regression gate, run by CI on every commit.
#
# Generates the deterministic synthetic session (fixed seed, so the input
# is byte-identical everywhere) and replays it through the full pipeline,
# then asserts the properties that must not regress:
#
#   1. Correctness of the closed loop: the injected 400 ms cross-venue
#      lead is recovered, with high correlation -- and the independent
#      event-study cross-check plus the two-method consensus agree that
#      Kalshi leads, so the headline finding is verified end to end, not
#      just by one method.
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
       confirmed follow_z agree consensus_kalshi \
       surv_alive_monotone surv_alive_bounded surv_nonneg \
    <<EOF
$(printf '%s' "$report" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ev = d["events"][0]
e = ev["lead_lag"]
es = ev["event_study"]
alive = [ev["survival_50ms_episodes"], ev["survival_100ms_episodes"],
         ev["survival_250ms_episodes"]]
surv = [ev["survival_open_mean_dollars"], ev["survival_50ms_mean_dollars"],
        ev["survival_100ms_mean_dollars"], ev["survival_250ms_mean_dollars"]]
print(e["lead_seconds"], e["correlation"],
      d["pipeline"]["records_per_sec"] / 1000.0,
      d["alloc"]["parse_per_msg"], d["alloc"]["book_per_msg"],
      d["malformed"], d["malformed_lines"], d["gaps"],
      int(es["lead_confirmed"]), es["follow_rate_z"],
      int(ev["methods_agree"]), int(ev["consensus_leader"] == "kalshi"),
      int(alive[0] >= alive[1] >= alive[2]),
      int(alive[0] <= ev["crossable_episodes"]),
      int(min(surv) >= 0.0))
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
# The independent cross-check and the two-method consensus must agree with
# cross-correlation on the injected lead; if either regresses, the headline
# finding ("two unrelated methods agree Kalshi leads") is no longer true.
check "event study confirms" "$confirmed"     "v == 1"
check "follow-rate z"        "$follow_z"      "v >= 1.96"
check "methods agree"        "$agree"         "v == 1"
check "consensus = kalshi"   "$consensus_kalshi" "v == 1"
check "malformed messages"   "$malformed"     "v == 0"
check "broken feedlog lines" "$bad_lines"     "v == 0"
check "sequence gaps"        "$gaps"          "v == 0"
check "parse allocs per msg" "$parse_per_msg" "v <= $MAX_PARSE_PER_MSG"
check "book allocs per msg"  "$books_per_msg" "v <= $MAX_BOOKS_PER_MSG"
check "throughput (k rec/s)" "$krps"          "v >= $MIN_KRPS"
# Reaction-latency ladder invariants: alive counts can only shrink as the
# delay grows, never exceed the episode count, and no surviving-edge mean
# may go negative (an expired episode is zero, a taker declines losses).
# These hold by construction; a violation means the fill logic broke.
check "survival alive monotone" "$surv_alive_monotone" "v == 1"
check "survival alive bounded"  "$surv_alive_bounded"  "v == 1"
check "survival means >= 0"     "$surv_nonneg"         "v == 1"

# Matching engine: the production book and the std::map reference book must
# agree on every fill over a long random order flow (agreed=0 exits nonzero
# inside the binary), and throughput must stay in the right order of
# magnitude. The floor is ~10x below developer hardware, same slack policy
# as the replay throughput gate.
lob=$("$BIN" lob-bench --ops 300000 --seed 5)
printf '%s\n' "$lob"
# `|| true` on every extraction below is load-bearing under `set -e`. A
# grep that matches nothing exits 1, which kills the script at the
# assignment - so if a bench renames an output key, CI fails with the log
# simply stopping and no line saying which value went missing. The check()
# function has a branch for exactly that case ("value missing from replay
# output") and it was unreachable. Letting the extraction yield an empty
# string is what makes it reachable.
lob_ops_per_sec=$(printf '%s' "$lob" | grep -oE 'ladder_ops_per_sec=[0-9.]+' | cut -d= -f2 || true)
lob_agreed=$(printf '%s' "$lob" | grep -oE 'agreed=[01]' | cut -d= -f2 || true)
check "lob books agree"      "$lob_agreed"       "v == 1"
check "lob ops/sec"          "$lob_ops_per_sec"  "v >= 2000000"

# Subscription fan-out. The conflating session's whole claim is that a slow
# consumer gets the CURRENT price rather than a backlog, so the property to
# gate is not the speedup - it is that no subscriber ever read a value a
# newer one had already superseded. worst_staleness_updates is that
# measurement, and until now nothing checked it: the matching engine's
# equivalent flag (agreed) was gated and this one was not, so a regression
# that made conflation deliver stale quotes would have passed CI while the
# speedup number stayed green.
#
# The speedup is gated too, but loosely. It is hardware-dependent and the
# point of the floor is only to catch the conflating path silently
# degrading to the synchronous one, not to pin a ratio.
fanout=$("$BIN" fanout-bench --subscribers 32 --slow 4 --updates 20000 --slow-us 50)
printf '%s\n' "$fanout"
fanout_stale=$(printf '%s' "$fanout" | grep -oE 'worst_staleness_updates=[0-9]+' | cut -d= -f2 || true)
fanout_speedup=$(printf '%s' "$fanout" | grep -oE 'speedup=[0-9.]+' | cut -d= -f2 || true)
fanout_delivered=$(printf '%s' "$fanout" | grep -oE 'delivered=[0-9]+' | cut -d= -f2 || true)
check "fanout staleness"     "$fanout_stale"     "v == 0"
check "fanout delivered > 0" "$fanout_delivered" "v > 0"
check "fanout speedup"       "$fanout_speedup"   "v >= 2"

if [ "$failures" -gt 0 ]; then
  echo "perf gate: $failures check(s) failed"
  exit 1
fi
echo "perf gate: all checks passed"
