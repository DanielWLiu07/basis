#pragma once

#include <string>

#include "bench/replay_harness.h"

namespace basis::bench {

// Rendering of a ReplayStats into the three forms the CLI offers: the
// human report, the --json object that scripts/perf_gate.sh and
// scripts/bench.sh both read, and the per-episode CSV.
//
// This lives apart from main.cpp because it is the bulk of what that file
// used to be -- over five hundred lines of formatting, a third of the
// translation unit -- and none of it is about dispatching a command. The
// exact output text is a contract: the gate and the bench summary parse
// it, so changes here are changes to a machine-readable interface.

// One row per crossed episode. False on an IO failure, which the caller
// reports rather than swallowing.
bool write_episodes_csv(const std::string& path, const ReplayStats& stats);

// The --json form. Allocation figures are passed in because they come from
// the counting resource the caller installed, not from the stats.
void print_stats_json(const ReplayStats& stats, double parse_per_msg,
                      double parse_bytes_per_msg, double book_per_msg);

// The human report.
void print_stats(const ReplayStats& stats);

}  // namespace basis::bench
