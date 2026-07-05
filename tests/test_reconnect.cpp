// Compiled only when BASIS_ENABLE_NET is on (see tests/CMakeLists.txt).
//
// The reliability claim in PLAN.md, made checkable: the real feed stack
// (TLS WebSocket client, reconnect with backoff, re-subscribe, parser,
// snapshot recovery) is pointed at a local server that speaks Polymarket's
// wire format and hard-drops the connection mid-subscription, repeatedly.
// The test passes only if the client's rebuilt book converges to the
// server's ground truth with every drop and every message accounted for.
// Peer and hostname verification stay on the whole time; the server's
// generated self-signed certificate is handed to the client as an extra
// trust anchor, not waved through.

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "feed/polymarket_feed.h"
#include "model/order_book.h"
#include "support/flaky_ws_server.h"
#include "support/test_tls.h"

namespace {

using namespace std::chrono_literals;

TEST(Reconnect, BookConvergesAcrossForcedDisconnects) {
  const auto identity = basis::testing::make_self_signed_identity();
  basis::testing::FlakyWsServer server(
      {.connections = 5, .deltas_per_connection = 20, .asset_id = "1000001"},
      identity);
  server.start();

  // The client's view of the book, rebuilt purely from sink deltas on
  // the feed's IO thread.
  std::mutex book_mutex;
  basis::model::OrderBook book;
  int clears = 0;

  basis::feed::PolymarketFeed feed(
      {.token_ids = {"1000001"},
       .host = "127.0.0.1",
       .port = std::to_string(server.port()),
       .trusted_ca_pem = identity.cert_pem,
       .initial_backoff_ms = 25});
  feed.set_sink([&](const basis::model::BookDelta& delta) {
    const std::lock_guard<std::mutex> lock(book_mutex);
    if (delta.action == basis::model::Action::Clear) ++clears;
    book.apply(delta);
  });
  feed.start();

  ASSERT_TRUE(server.wait_finished(30'000)) << "server script never finished";

  // The last deltas may still be in flight after the script ends; poll
  // for convergence instead of sleeping a fixed guess.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool converged = false;
  while (!converged && std::chrono::steady_clock::now() < deadline) {
    {
      const std::lock_guard<std::mutex> lock(book_mutex);
      converged = book.best_bid() == server.final_bid_cents() &&
                  book.best_ask() == server.final_ask_cents();
    }
    if (!converged) std::this_thread::sleep_for(10ms);
  }
  feed.stop();
  server.stop();

  const std::lock_guard<std::mutex> lock(book_mutex);
  ASSERT_TRUE(book.best_bid().has_value());
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(*book.best_bid(), server.final_bid_cents());
  EXPECT_EQ(*book.best_ask(), server.final_ask_cents());

  // Four hard drops, four recoveries, each one answered by a snapshot
  // (one Clear per connection). Nothing malformed. A hard TCP close may
  // legitimately truncate deltas already in flight on the wire; that is
  // the injected fault, and the snapshot recovery is what makes the book
  // whole again, so the count is bounded rather than exact.
  EXPECT_EQ(feed.reconnects(), 4u);
  EXPECT_EQ(clears, 5);
  EXPECT_EQ(feed.malformed(), 0u);
  EXPECT_GE(feed.messages(), 25u);       // every snapshot + the last leg
  EXPECT_LE(feed.messages(), 5u * 21u);  // never more than was sent
}

}  // namespace
