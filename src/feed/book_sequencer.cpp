#include "feed/book_sequencer.h"

namespace basis::feed {

void BookSequencer::on_snapshot(std::int64_t last_update_id) {
  ++stats_.snapshots;
  snapshot_id_ = last_update_id;
  last_applied_id_ = last_update_id;
  awaiting_first_ = true;
  state_ = State::Live;  // provisional: the first event must still straddle
}

BookSequencer::Decision BookSequencer::on_update(std::int64_t first_id,
                                                 std::int64_t last_id) {
  if (state_ == State::AwaitingSnapshot || state_ == State::ResyncRequired) {
    ++stats_.buffered;
    return Decision::Buffer;
  }

  if (awaiting_first_) {
    // Wholly at or before the snapshot: the snapshot already contains it.
    if (last_id <= snapshot_id_) {
      ++stats_.discarded;
      return Decision::Discard;
    }
    // The join condition. An event that begins after the snapshot's next
    // id means everything between was missed, so the snapshot is too old
    // to join from and a newer one is required.
    if (first_id > snapshot_id_ + 1) {
      ++stats_.stale_snapshots;
      state_ = State::ResyncRequired;
      return Decision::Gap;
    }
    awaiting_first_ = false;
    last_applied_id_ = last_id;
    ++stats_.applied;
    return Decision::Apply;
  }

  // Steady state: ids must continue with no hole.
  if (first_id == last_applied_id_ + 1) {
    last_applied_id_ = last_id;
    ++stats_.applied;
    return Decision::Apply;
  }
  // A replayed or duplicated event is harmless; it is already applied.
  if (last_id <= last_applied_id_) {
    ++stats_.discarded;
    return Decision::Discard;
  }
  // Anything else means at least one event never arrived. The book cannot
  // be repaired from the stream, only rebuilt.
  ++stats_.gaps;
  state_ = State::ResyncRequired;
  return Decision::Gap;
}

void BookSequencer::request_resync() {
  state_ = State::AwaitingSnapshot;
  awaiting_first_ = false;
  snapshot_id_ = -1;
  last_applied_id_ = -1;
}

}  // namespace basis::feed
