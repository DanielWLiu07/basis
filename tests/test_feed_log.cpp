#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "feed/feed_log.h"

using basis::feed::FeedLogReader;
using basis::feed::FeedLogWriter;
using basis::model::Venue;

namespace {

std::string temp_path(const char* name) {
  return testing::TempDir() + name;
}

}  // namespace

TEST(FeedLog, RoundTripsRecordsVerbatim) {
  const auto path = temp_path("roundtrip.feedlog");
  {
    FeedLogWriter w(path);
    ASSERT_TRUE(w.ok());
    EXPECT_TRUE(w.write({1000, Venue::Kalshi, R"({"type":"orderbook_delta"})"}));
    EXPECT_TRUE(w.write({2000, Venue::Polymarket,
                         "{\"note\":\"tab\there is payload, not framing\"}"}));
  }
  FeedLogReader r(path);
  ASSERT_TRUE(r.ok());

  auto rec = r.next();
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->recv_ns, 1000);
  EXPECT_EQ(rec->venue, Venue::Kalshi);
  EXPECT_EQ(rec->payload, R"({"type":"orderbook_delta"})");

  rec = r.next();
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->venue, Venue::Polymarket);
  EXPECT_EQ(rec->payload, "{\"note\":\"tab\there is payload, not framing\"}");

  EXPECT_FALSE(r.next().has_value());
  EXPECT_EQ(r.malformed_lines(), 0u);
}

TEST(FeedLog, MalformedLinesAreCountedAndSkipped) {
  const auto path = temp_path("malformed.feedlog");
  {
    std::ofstream out(path);
    out << "not a record\n";
    out << "abc\tkalshi\t{}\n";        // bad timestamp
    out << "123\tnasdaq\t{}\n";        // unknown venue
    out << "456\tkalshi\t{\"ok\":1}\n";  // good
  }
  FeedLogReader r(path);
  const auto rec = r.next();
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->recv_ns, 456);
  EXPECT_FALSE(r.next().has_value());
  EXPECT_EQ(r.malformed_lines(), 3u);
}

TEST(FeedLog, NegativeTimestampIsMalformed) {
  const auto path = temp_path("negative-ts.feedlog");
  {
    std::ofstream out(path);
    out << "-5\tkalshi\t{}\n";
    out << "5\tkalshi\t{}\n";
  }
  FeedLogReader r(path);
  const auto rec = r.next();
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->recv_ns, 5);
  EXPECT_EQ(r.malformed_lines(), 1u);
}

TEST(FeedLog, OverlongLineIsCountedAndSkipped) {
  const auto path = temp_path("overlong.feedlog");
  {
    std::ofstream out(path);
    // One hostile line well past the reader's bound must not balloon
    // memory or end the replay; the next record still parses.
    out << "1\tkalshi\t" << std::string(2 * FeedLogReader::kMaxLineBytes, 'x')
        << "\n";
    out << "2\tpolymarket\t{}\n";
  }
  FeedLogReader r(path);
  const auto rec = r.next();
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->recv_ns, 2);
  EXPECT_FALSE(r.next().has_value());
  EXPECT_EQ(r.malformed_lines(), 1u);
}

TEST(FeedLog, WriterRejectsUnframablePayload) {
  const auto path = temp_path("reject.feedlog");
  FeedLogWriter w(path);
  EXPECT_FALSE(w.write({1, Venue::Kalshi, "line one\nline two"}));
  EXPECT_TRUE(w.write({2, Venue::Kalshi, "{}"}));
}

TEST(FeedLog, MissingFileReportsNotOk) {
  FeedLogReader r(temp_path("does-not-exist.feedlog"));
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(r.next().has_value());
}
