#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include "model/types.h"

namespace basis::feed {

// Capture format (.feedlog): one record per line, tab-separated.
//
//   recv_ns <TAB> venue <TAB> raw venue JSON (verbatim, single line)
//
// recv_ns is the receive timestamp (socket-read time live, simulated clock
// for synthetic sessions). The payload is stored byte-for-byte so replay
// exercises the exact parser code the live path uses. Everything after the
// second tab is payload, so tabs inside JSON strings are safe.
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
  explicit FeedLogReader(const std::string& path);

  bool ok() const { return open_; }

  // Next well-formed record, or nullopt at end of file. Malformed lines are
  // skipped but counted, never silently dropped.
  std::optional<FeedLogRecord> next();

  std::uint64_t malformed_lines() const { return malformed_; }

 private:
  std::ifstream in_;
  bool open_ = false;
  std::uint64_t malformed_ = 0;
};

}  // namespace basis::feed
