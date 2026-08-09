#include <gtest/gtest.h>

#include "feed/book_sequencer.h"

using basis::feed::BookSequencer;
using D = BookSequencer::Decision;
using S = BookSequencer::State;

TEST(BookSequencer, BuffersUntilTheSnapshotArrives) {
  BookSequencer seq;
  EXPECT_EQ(seq.state(), S::AwaitingSnapshot);
  // Nothing can be applied before there is a book to apply it to, and the
  // events cannot be dropped either: the snapshot may predate them.
  EXPECT_EQ(seq.on_update(10, 12), D::Buffer);
  EXPECT_EQ(seq.on_update(13, 15), D::Buffer);
  EXPECT_EQ(seq.stats().buffered, 2u);
}

TEST(BookSequencer, DiscardsWhatTheSnapshotAlreadyContains) {
  BookSequencer seq;
  seq.on_snapshot(100);
  // Wholly at or before lastUpdateId: already reflected in the snapshot.
  EXPECT_EQ(seq.on_update(90, 95), D::Discard);
  EXPECT_EQ(seq.on_update(96, 100), D::Discard);
  // The straddling event joins the stream.
  EXPECT_EQ(seq.on_update(99, 104), D::Apply);
  EXPECT_EQ(seq.last_applied_id(), 104);
  EXPECT_EQ(seq.stats().discarded, 2u);
}

TEST(BookSequencer, AcceptsTheExactBoundaryStraddle) {
  // U == lastUpdateId + 1 is the tightest legal join.
  BookSequencer seq;
  seq.on_snapshot(100);
  EXPECT_EQ(seq.on_update(101, 105), D::Apply);
  EXPECT_EQ(seq.state(), S::Live);
}

TEST(BookSequencer, RejectsASnapshotOlderThanTheStream) {
  // The first event begins past lastUpdateId + 1, so everything between
  // the snapshot and this event was missed: the snapshot cannot be joined
  // from and a newer one is required. Silently applying here is exactly
  // how a book goes wrong while looking fine.
  BookSequencer seq;
  seq.on_snapshot(100);
  EXPECT_EQ(seq.on_update(150, 160), D::Gap);
  EXPECT_EQ(seq.state(), S::ResyncRequired);
  EXPECT_EQ(seq.stats().stale_snapshots, 1u);
  EXPECT_EQ(seq.stats().applied, 0u);
}

TEST(BookSequencer, RequiresUnbrokenContinuityOnceLive) {
  BookSequencer seq;
  seq.on_snapshot(100);
  ASSERT_EQ(seq.on_update(101, 105), D::Apply);
  ASSERT_EQ(seq.on_update(106, 110), D::Apply);
  // One event never arrived: 111 is missing.
  EXPECT_EQ(seq.on_update(112, 115), D::Gap);
  EXPECT_EQ(seq.state(), S::ResyncRequired);
  EXPECT_EQ(seq.stats().gaps, 1u);
  // After a gap nothing is applied until a fresh snapshot lands.
  EXPECT_EQ(seq.on_update(116, 120), D::Buffer);
}

TEST(BookSequencer, DuplicateAndReplayedEventsAreDiscardedNotGaps) {
  BookSequencer seq;
  seq.on_snapshot(100);
  ASSERT_EQ(seq.on_update(101, 105), D::Apply);
  // A redelivery of something already applied is harmless.
  EXPECT_EQ(seq.on_update(101, 105), D::Discard);
  EXPECT_EQ(seq.on_update(95, 99), D::Discard);
  EXPECT_EQ(seq.stats().gaps, 0u);
  // And the stream continues normally afterwards.
  EXPECT_EQ(seq.on_update(106, 108), D::Apply);
}

TEST(BookSequencer, ResyncRestartsTheWholeProcedure) {
  BookSequencer seq;
  seq.on_snapshot(100);
  ASSERT_EQ(seq.on_update(101, 105), D::Apply);
  ASSERT_EQ(seq.on_update(120, 125), D::Gap);

  seq.request_resync();
  EXPECT_EQ(seq.state(), S::AwaitingSnapshot);
  EXPECT_EQ(seq.on_update(126, 130), D::Buffer);  // buffering again

  // A newer snapshot joins cleanly and the book is trustworthy again.
  seq.on_snapshot(129);
  EXPECT_EQ(seq.on_update(126, 130), D::Apply);
  EXPECT_EQ(seq.state(), S::Live);
  EXPECT_EQ(seq.stats().snapshots, 2u);
}
