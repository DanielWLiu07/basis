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
  // Venue subscription id the message arrived on, 0 when absent. Kalshi
  // gap recovery needs it: re-subscribing a live subscription is rejected,
  // so the feed unsubscribes this sid first.
  std::uint64_t sid = 0;

  // Integrity-hash accounting for book snapshots that carry one
  // (Polymarket), counted per book because one wire message can bundle
  // several. Unverifiable means the snapshot omits fields the venue
  // hashes over, which its periodic refresh form does
  // (docs/api_integration.md); those are counted, never guessed at.
  std::uint32_t hashes_verified = 0;
  std::uint32_t hashes_mismatched = 0;
  std::uint32_t hashes_unverifiable = 0;

  // Price levels the message carried that this engine's price model cannot
  // represent, dropped from the deltas but counted here rather than
  // silently discarded.
  //
  // This is not a hypothetical. A real Coinbase level2 snapshot for BTC-USD
  // carries 45,177 levels, five of which are asks above $20,000,000 (the
  // deepest at $138,991,023) -- resting joke orders two thousand times the
  // market price, which the venue is happy to publish. They cannot fit in
  // an int32 of cents. The distinction matters because the alternative,
  // treating an unrepresentable level as a malformed message, threw away
  // the entire book image over levels that can never be top of book, and
  // left every later diff applying to an empty book.
  std::uint32_t levels_unrepresentable = 0;
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
