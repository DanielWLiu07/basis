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

#include "feed_live/polymarket_feed.h"
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

// The counterpart that gives the test above its teeth: the same server,
// but the client is not told to trust its self-signed certificate. With
// verification actually enforced, the handshake fails on every attempt
// and not one book delta arrives. If verification were off (the settings
// applied to the context after the SSL object was built, say), the
// untrusted certificate would sail through and deltas would flow.
TEST(Reconnect, RejectsAnUntrustedCertificate) {
  const auto identity = basis::testing::make_self_signed_identity();
  basis::testing::FlakyWsServer server(
      {.connections = 30, .deltas_per_connection = 5, .asset_id = "1000001"},
      identity);
  server.start();

  std::mutex mutex;
  int deltas = 0;

  basis::feed::PolymarketFeed feed(
      {.token_ids = {"1000001"},
       .host = "127.0.0.1",
       .port = std::to_string(server.port()),
       // trusted_ca_pem deliberately omitted: the server's certificate is
       // self-signed and in no trust store this client has.
       .initial_backoff_ms = 25});
  feed.set_sink([&](const basis::model::BookDelta&) {
    const std::lock_guard<std::mutex> lock(mutex);
    ++deltas;
  });
  feed.start();

  // Give the client well over a second of reconnect attempts; a bypassed
  // check would have delivered the first snapshot within tens of ms.
  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (deltas > 0) break;  // fail fast if the cert was accepted
    }
    std::this_thread::sleep_for(20ms);
  }
  feed.stop();
  server.stop();

  const std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(deltas, 0);
  EXPECT_EQ(feed.messages(), 0u);
}


// The fault the reconnect path above cannot see.
//
// Every recovery in this file so far is driven by a read error: the server
// hard-closes and the client's blocking read fails. A network that goes
// away without closing anything - a half-open connection, a middlebox that
// forgets the flow - produces no error at all. The read parks forever and
// the client reports perfect health while receiving nothing.
//
// That is not hypothetical. A 45 minute two-venue capture had Coinbase go
// silent 19 minutes in and stay silent for the remaining 25, with zero
// reconnects logged, while Binance recovered twice across the same outage
// because its server actually closes connections.
//
// Here the server sends its deltas and then simply stops talking, holding
// the socket open. Only the idle watchdog can end that.
TEST(Reconnect, IdleWatchdogBreaksASilentButOpenConnection) {
  const auto identity = basis::testing::make_self_signed_identity();
  basis::testing::FlakyWsServer server(
      {.connections = 2,
       .deltas_per_connection = 2,
       .asset_id = "1000001",
       .stall_ms = 1500},
      identity);
  server.start();

  basis::feed::PolymarketFeed feed(
      {.token_ids = {"1000001"},
       .host = "127.0.0.1",
       .port = std::to_string(server.port()),
       .trusted_ca_pem = identity.cert_pem,
       .initial_backoff_ms = 25,
       // Far below the server's 1500ms silence, so the watchdog must fire
       // during the first stall rather than after the server gives up.
       .idle_timeout_ms = 300});
  feed.start();

  // Poll for the forced reconnect instead of sleeping a fixed guess.
  // Both counters, not just stalls: the watchdog increments stalls the
  // moment it breaks the socket, but reconnects only rises once the new
  // connection is up, so waiting on stalls alone races the recovery.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while ((feed.stalls() == 0 || feed.reconnects() == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  const auto stalls = feed.stalls();
  const auto reconnects = feed.reconnects();
  feed.stop();
  server.stop();

  // The watchdog noticed the silence and forced the reconnect that a
  // read error never would have. Before it existed this was 0 and the
  // client sat on a dead socket until the process ended.
  EXPECT_GE(stalls, 1u);
  EXPECT_GE(reconnects, 1u);
}

}  // namespace
