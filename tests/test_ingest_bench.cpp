#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "cli/commands.h"

namespace {

// One-line payloads, because the feedlog frames one record per line.
constexpr std::string_view kBinance =
    R"({"stream":"bnbusdt@bookTicker","data":{"u":718455119,"s":"BNBUSDT",)"
    R"("b":"602.55000000","B":"4.60000000",)"
    R"("a":"602.56000000","A":"33.80000000"}})";

constexpr std::string_view kPolymarket =
    R"({"event_type":"book","asset_id":"7132107","market":"0xabc123",)"
    R"("bids":[{"price":"0.44","size":"1200.5"}],)"
    R"("asks":[{"price":"0.46","size":"800"}],)"
    R"("timestamp":"1750000000000","hash":"deadbeef"})";

// Writes a feedlog and returns its path. Records are (venue, payload); the
// receive timestamps are spaced 1 ms apart so the span is non-zero.
// The path is built from the test's own name rather than tmpnam, which is
// deprecated and which the warning-as-error build rejects.
std::string write_log(const std::string& name,
                      const std::vector<std::pair<const char*,
                                                  std::string_view>>& rows) {
  const std::string path =
      std::string(::testing::TempDir()) + "basis_ingest" + name;
  std::ofstream out(path);
  std::int64_t ts = 1'000'000'000;
  for (const auto& [venue, payload] : rows) {
    out << ts << '\t' << venue << '\t' << payload << '\n';
    ts += 1'000'000;
  }
  return path;
}

int run(const std::string& path) {
  const std::vector<std::string_view> args{path};
  return basis::cli::run_ingest_bench_cmd(args);
}

}  // namespace

// The regression this file exists for. ingest-bench used to hardcode the
// Binance parser and ignore the venue column, so a capture from any other
// venue parsed to nothing and the command still reported a throughput and
// a headroom multiple - a benchmark measuring how fast simdjson rejects a
// message it was never given.
TEST(IngestBench, ParsesEachRecordWithItsOwnVenuesParser) {
  const auto path = write_log("_poly.feedlog", {{"polymarket", kPolymarket}});
  EXPECT_EQ(run(path), 0);
  std::remove(path.c_str());
}

TEST(IngestBench, HandlesACaptureThatInterleavesVenues) {
  const auto path = write_log("_mixed.feedlog", {{"binance", kBinance},
                                                 {"polymarket", kPolymarket},
                                                 {"binance", kBinance}});
  EXPECT_EQ(run(path), 0);
  std::remove(path.c_str());
}

// The guard, stated as a test: a run that produced no deltas did no
// pipeline work, so its rates describe rejection cost and must not be
// reported as a result. A payload labelled with the wrong venue is the
// cheapest way to produce that state on purpose.
TEST(IngestBench, FailsWhenNothingParsedRatherThanReportingARate) {
  const auto path = write_log("_mislabelled.feedlog",
                              {{"binance", kPolymarket}});
  EXPECT_NE(run(path), 0);
  std::remove(path.c_str());
}

TEST(IngestBench, FailsOnAnEmptyOrMissingCapture) {
  EXPECT_NE(run("/nonexistent/path/to/a.feedlog"), 0);
  const auto path = write_log("_empty.feedlog", {});
  EXPECT_NE(run(path), 0);
  std::remove(path.c_str());
}
