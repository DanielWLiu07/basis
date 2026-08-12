#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "model/types.h"

namespace basis::feed {

// Capture format (.feedlog): one record per line, tab-separated.
//
//   recv_ns <TAB> venue <TAB> raw venue JSON (verbatim, single line)
//
// recv_ns is the receive timestamp (socket-read time live, simulated clock
// for synthetic sessions); negative values are rejected as malformed. The
// payload is stored byte-for-byte so replay exercises the exact parser code
// the live path uses. Everything after the second tab is payload, so tabs
// inside JSON strings are safe.
struct FeedLogRecord {
  std::int64_t recv_ns = 0;
  model::Venue venue = model::Venue::Kalshi;
  std::string payload;
};

class FeedLogWriter {
 public:
  explicit FeedLogWriter(const std::string& path);

  bool ok() const { return out_.good(); }

  // False on IO failure or a payload that cannot be framed (embedded
  // newline); the caller counts rejects, per the no-silent-drop rule.
  bool write(const FeedLogRecord& record);

 private:
  std::ofstream out_;
};

class FeedLogReader {
 public:
  // Bounding the line keeps one corrupt or hostile file from ballooning
  // RSS. This was 1 MiB under the assumption that no real message comes
  // close, which turned out to be wrong: a Coinbase level2 snapshot for
  // BTC-USD is 45,177 price levels and 1.13 MiB on the wire, so the cap
  // silently rejected the one message the rest of the stream is diffed
  // against. 8 MiB clears that with room for a deeper book while still
  // bounding the damage a bad file can do.
  static constexpr std::size_t kMaxLineBytes = 8u << 20;  // 8 MiB

  explicit FeedLogReader(const std::string& path);

  bool ok() const { return open_; }

  // Next well-formed record, or nullopt at end of file. Malformed lines
  // (bad framing, negative timestamp, over-long) are skipped but counted,
  // never silently dropped.
  std::optional<FeedLogRecord> next();

  std::uint64_t malformed_lines() const { return malformed_; }

 private:
  std::ifstream in_;
  std::vector<char> buffer_;
  bool open_ = false;
  std::uint64_t malformed_ = 0;
};

}  // namespace basis::feed
