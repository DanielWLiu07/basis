#pragma once

#include <cstdint>

namespace basis::feed {

// Sequencing for a venue that publishes a book as a diff stream plus an
// out-of-band snapshot. This is the part a feed handler cannot get from
// the venue's API: the venue sends you updates, and whether the book you
// build out of them is the book the venue has is your problem.
//
// The protocol it encodes is Binance's, and it is representative. Each
// depth event covers the update-id range [U, u]. A snapshot is taken
// separately over REST and carries a lastUpdateId. Reconstruction is only
// correct if all of the following hold:
//
//   Events that arrive before the snapshot is in hand must be buffered,
//   because the snapshot may turn out to predate them.
//   Events wholly at or before the snapshot (u <= lastUpdateId) are
//   already reflected in it and must be discarded, not reapplied.
//   The first event applied must straddle the snapshot: U <= lastUpdateId
//   + 1 <= u. If none does, the snapshot is older than the buffer and the
//   whole attempt must restart - the gap is unrecoverable from this data.
//   Afterwards every event must continue exactly: U == previous u + 1. A
//   single missing event means the book has silently diverged, and the
//   only correct response is to stop trusting it and resynchronize.
//
// The failure this exists to prevent is not a crash. It is a book that
// looks completely normal and is wrong, which is worse, because every
// number downstream inherits the error without any symptom.
//
// This class decides; it does not apply. It takes update ids and returns
// what the caller should do with the event, so it can be tested
// exhaustively without an order book, and so the same logic serves any
// venue with this shape of protocol.
class BookSequencer {
 public:
  enum class State : std::uint8_t {
    AwaitingSnapshot,  // buffering; no book yet
    Live,              // book is coherent and continuous
    ResyncRequired,    // a gap was seen; the book is not trustworthy
  };

  enum class Decision : std::uint8_t {
    Buffer,   // hold it: the snapshot has not arrived (or is being redone)
    Apply,    // in sequence; apply to the book
    Discard,  // already covered by the snapshot, or a duplicate
    Gap,      // continuity broken; caller must drop the book and resync
  };

  struct Stats {
    std::uint64_t buffered = 0;
    std::uint64_t applied = 0;
    std::uint64_t discarded = 0;
    std::uint64_t gaps = 0;
    std::uint64_t snapshots = 0;
    // Snapshots that no buffered event straddled, so the sequencer could
    // not join the stream and had to ask for a newer one.
    std::uint64_t stale_snapshots = 0;
  };

  // A snapshot has been obtained at `last_update_id`. The next in-sequence
  // event is the one that straddles it.
  void on_snapshot(std::int64_t last_update_id);

  // Classify one depth event covering update ids [first_id, last_id].
  Decision on_update(std::int64_t first_id, std::int64_t last_id);

  // Called after a Gap or a stale snapshot: the caller has dropped its
  // book and will fetch a new snapshot, so buffer again until it lands.
  void request_resync();

  State state() const { return state_; }
  std::int64_t last_applied_id() const { return last_applied_id_; }
  const Stats& stats() const { return stats_; }

 private:
  State state_ = State::AwaitingSnapshot;
  std::int64_t snapshot_id_ = -1;
  std::int64_t last_applied_id_ = -1;
  bool awaiting_first_ = false;  // snapshot in hand, no event applied yet
  Stats stats_;
};

}  // namespace basis::feed
