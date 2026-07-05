#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include "core/bounded_queue.h"
#include "model/book_delta.h"

namespace {

using basis::core::BoundedQueue;

TEST(BoundedQueue, FifoWithinOneProducer) {
  BoundedQueue<int> q(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
  for (int i = 0; i < 4; ++i) EXPECT_EQ(*q.pop(), i);
  EXPECT_EQ(q.pushed(), 4u);
  EXPECT_EQ(q.popped(), 4u);
  EXPECT_EQ(q.high_water(), 4u);
}

TEST(BoundedQueue, FullQueueBlocksThePushAndCountsIt) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  std::thread producer([&] { EXPECT_TRUE(q.push(2)); });
  // blocked_pushes increments before the wait, so it doubles as the
  // deterministic signal that the producer really hit a full queue.
  while (q.blocked_pushes() == 0) std::this_thread::yield();
  EXPECT_EQ(*q.pop(), 1);
  EXPECT_EQ(*q.pop(), 2);
  producer.join();
  EXPECT_EQ(q.blocked_pushes(), 1u);
}

TEST(BoundedQueue, CloseWakesABlockedPopAndRejectsNewPushes) {
  BoundedQueue<int> q(4);
  std::thread consumer([&] { EXPECT_FALSE(q.pop().has_value()); });
  q.close();
  consumer.join();
  EXPECT_FALSE(q.push(1));
  EXPECT_EQ(q.pushed(), 0u);
}

TEST(BoundedQueue, CloseDrainsWhatItHolds) {
  BoundedQueue<int> q(4);
  EXPECT_TRUE(q.push(7));
  EXPECT_TRUE(q.push(8));
  q.close();
  // Nothing already queued is lost to shutdown.
  EXPECT_EQ(*q.pop(), 7);
  EXPECT_EQ(*q.pop(), 8);
  EXPECT_FALSE(q.pop().has_value());
}

// The live pipeline's shape: two feed IO threads producing, one analytics
// thread consuming, through a queue much smaller than the item count so
// the full/empty paths are exercised constantly. TSan runs this in CI.
TEST(BoundedQueue, TwoProducersOneConsumerLosesNothing) {
  constexpr int kPerProducer = 50'000;
  BoundedQueue<std::int64_t> q(64);

  auto produce = [&](std::int64_t base) {
    for (int i = 0; i < kPerProducer; ++i) {
      ASSERT_TRUE(q.push(base + i));
    }
  };
  std::thread a(produce, 0);
  std::thread b(produce, static_cast<std::int64_t>(1) << 32);

  std::set<std::int64_t> seen;
  std::thread consumer([&] {
    while (auto v = q.pop()) seen.insert(*v);
  });

  a.join();
  b.join();
  q.close();
  consumer.join();

  EXPECT_EQ(seen.size(), 2u * kPerProducer);  // distinct: no loss, no dup
  EXPECT_EQ(q.pushed(), 2u * kPerProducer);
  EXPECT_EQ(q.popped(), 2u * kPerProducer);
  EXPECT_LE(q.high_water(), 64u);
}

TEST(OwnedBookDelta, OwnsItsMarketAcrossMoves) {
  // A market id long past the small-string optimization, like a real
  // Polymarket token id, so a dangling view would point at freed heap.
  const std::string long_id(77, '7');
  basis::model::BookDelta wire{.venue = basis::model::Venue::Polymarket,
                               .market = long_id,
                               .price_cents = 47,
                               .size = 100};

  basis::model::OwnedBookDelta owned(wire);
  std::vector<basis::model::OwnedBookDelta> moved_around;
  for (int i = 0; i < 32; ++i) {
    moved_around.push_back(std::move(owned));  // forces reallocations
    owned = std::move(moved_around.back());
  }

  const auto view = owned.view();
  EXPECT_EQ(view.market, long_id);
  EXPECT_EQ(view.price_cents, 47);
  EXPECT_EQ(view.market.data(), owned.market.data());  // aliases owner
}

}  // namespace
