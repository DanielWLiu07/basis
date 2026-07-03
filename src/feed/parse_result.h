#pragma once

#include <memory_resource>
#include <vector>

#include "model/book_delta.h"

namespace basis::feed {

enum class ParseStatus {
  Ok,         // message produced canonical deltas
  Ignored,    // valid but irrelevant (subscription acks, heartbeats, ...)
  Malformed,  // structurally broken; counted upstream, never silently dropped
};

// Output of parsing one raw venue WebSocket message.
//
// The deltas vector allocates from the memory_resource handed to parse(),
// so a caller replaying millions of messages can back it with an arena it
// releases per message instead of hitting the global heap. The result (and
// the market views inside it, see BookDelta) is valid until the next
// parse() on the same parser or until the arena is released, whichever
// comes first.
struct ParseResult {
  explicit ParseResult(
      std::pmr::memory_resource* mr = std::pmr::get_default_resource())
      : deltas(mr) {}

  std::pmr::vector<model::BookDelta> deltas;
  ParseStatus status = ParseStatus::Ignored;
  // True when stream integrity broke (sequence gap). The parser has already
  // prepended a Clear delta so the stale book cannot survive; the live feed
  // additionally uses this to trigger a re-snapshot.
  bool gap = false;
};

constexpr const char* to_string(ParseStatus s) {
  switch (s) {
    case ParseStatus::Ok:        return "ok";
    case ParseStatus::Ignored:   return "ignored";
    case ParseStatus::Malformed: return "malformed";
  }
  return "?";
}

}  // namespace basis::feed
