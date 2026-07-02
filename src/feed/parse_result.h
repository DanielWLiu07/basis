#pragma once

#include <vector>

#include "model/book_delta.h"

namespace basis::feed {

enum class ParseStatus {
  Ok,         // message produced canonical deltas
  Ignored,    // valid but irrelevant (subscription acks, heartbeats, ...)
  Malformed,  // structurally broken; counted upstream, never silently dropped
};

// Output of parsing one raw venue WebSocket message.
struct ParseResult {
  std::vector<model::BookDelta> deltas;
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
