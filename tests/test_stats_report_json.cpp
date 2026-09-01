#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "bench/stats_report.h"

namespace {

// print_stats_json writes to stdout, so the test captures it by swapping
// the file descriptor underneath.
//
// Not freopen: restoring with freopen needs a path to reopen, and the
// obvious one, /dev/tty, does not exist on a CI runner. There the restore
// silently fails, stdout stays pointed at the temp file, and every later
// test in the binary writes into it - a broken capture that presents as
// missing output somewhere unrelated. dup/dup2 restores the descriptor
// that was actually there, whatever it was.
std::string capture_json(const basis::bench::ReplayStats& stats) {
  const std::string path = std::string(::testing::TempDir()) + "basis_json.txt";
  std::fflush(stdout);
  const int saved = dup(STDOUT_FILENO);
  FILE* tmp = std::fopen(path.c_str(), "w");
  if (tmp == nullptr || saved < 0) return {};
  dup2(fileno(tmp), STDOUT_FILENO);

  basis::bench::print_stats_json(stats, 1.5, 96.0, 2.5);

  std::fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  std::fclose(tmp);

  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  std::remove(path.c_str());
  return ss.str();
}

basis::bench::ReplayStats one_event_stats() {
  basis::bench::ReplayStats s;
  s.records = 10;
  basis::bench::ReplayStats::EventReport ev;
  ev.event_id = "synthetic-demo";
  ev.basis_samples = 5;
  s.events.push_back(ev);
  return s;
}

}  // namespace

// scripts/perf_gate.sh reads these exact paths out of `replay --json` and
// fails the build on their values. Nothing checked that they still exist,
// so renaming one turned a schema break into a CI failure that reads like
// a performance regression, discovered on a pull request rather than in a
// unit test.
//
// This is a contract test, not a formatting test: it asserts the key names
// the gate depends on are emitted, and deliberately says nothing about
// their values, layout, or ordering.
TEST(StatsReportJson, EmitsEveryKeyThePerfGateReads) {
  const std::string json = capture_json(one_event_stats());
  ASSERT_FALSE(json.empty()) << "captured nothing from print_stats_json";

  // Top level.
  for (const char* key : {"\"events\"", "\"pipeline\"", "\"alloc\"",
                          "\"malformed\"", "\"malformed_lines\"",
                          "\"gaps\"", "\"records_per_sec\"",
                          "\"parse_per_msg\"", "\"book_per_msg\""}) {
    EXPECT_NE(json.find(key), std::string::npos)
        << "perf_gate.sh reads " << key << " and it is no longer emitted";
  }

  // Per event.
  for (const char* key : {"\"lead_lag\"", "\"event_study\"",
                          "\"lead_seconds\"", "\"correlation\"",
                          "\"lead_confirmed\"", "\"follow_rate_z\"",
                          "\"methods_agree\"", "\"consensus_leader\"",
                          "\"crossable_episodes\"",
                          "\"survival_open_mean_dollars\"",
                          "\"survival_50ms_mean_dollars\"",
                          "\"survival_100ms_mean_dollars\"",
                          "\"survival_250ms_mean_dollars\"",
                          "\"survival_50ms_episodes\"",
                          "\"survival_100ms_episodes\"",
                          "\"survival_250ms_episodes\""}) {
    EXPECT_NE(json.find(key), std::string::npos)
        << "perf_gate.sh reads " << key << " and it is no longer emitted";
  }
}

// The paced-replay block is emitted only when a --speed was given, and
// perf_gate.sh reads three paths out of it to check the coordinated
// omission invariant. Same contract, same reason: renaming one turns a
// schema break into a gate that reports a missing value.
TEST(StatsReportJson, EmitsThePacedKeysThePerfGateReads) {
  basis::bench::ReplayStats s = one_event_stats();
  s.replay_speed = 1.0;          // what gates the block
  s.response_latency.count = 3;  // non-empty so the numbers are real
  const std::string json = capture_json(s);
  for (const char* key : {"\"response_us\"", "\"pacing\"", "\"speed\"",
                          "\"records_late\"", "\"pacer_overshoots\""}) {
    EXPECT_NE(json.find(key), std::string::npos)
        << "perf_gate.sh reads " << key << " and it is no longer emitted";
  }
}

// The same block must NOT appear on an unpaced run, or the gate would read
// a response time from a measurement that never paced anything.
TEST(StatsReportJson, OmitsThePacedKeysWhenNothingWasPaced) {
  const std::string json = capture_json(one_event_stats());
  EXPECT_EQ(json.find("\"response_us\""), std::string::npos);
  EXPECT_EQ(json.find("\"pacing\""), std::string::npos);
}

// A gate that indexes events[0] cannot run against a report with no
// events, so an empty run must still emit the array rather than omitting
// the key - the difference between "no events" and "schema changed".
TEST(StatsReportJson, EmitsAnEventsArrayEvenWithNoEvents) {
  const std::string json = capture_json(basis::bench::ReplayStats{});
  EXPECT_NE(json.find("\"events\""), std::string::npos);
}
