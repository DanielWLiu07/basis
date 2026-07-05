#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "support/test_tls.h"

namespace basis::testing {

// A local TLS WebSocket server that speaks Polymarket's market-channel
// wire format and injects the fault the reconnect path exists for: after
// a scripted number of deltas it hard-closes the TCP socket, no
// WebSocket close frame, mid-subscription. Every accepted connection
// answers the subscribe with a fresh snapshot of the server's book, the
// same recovery contract the real venue provides.
//
// The book itself is a deterministic two-level walk, so the test can
// compare the client's rebuilt book against ground truth after any
// number of drops.
class FlakyWsServer {
 public:
  struct Config {
    int connections = 5;           // accepts before the script finishes
    int deltas_per_connection = 20;
    std::string asset_id = "1000001";
  };

  FlakyWsServer(Config config, TlsIdentity identity);
  ~FlakyWsServer();

  void start();
  void stop();

  std::uint16_t port() const { return port_; }

  // Blocks until the scripted session completes (all connections served).
  bool wait_finished(int timeout_ms);

  // Ground truth after the last scripted delta. Only valid once
  // wait_finished() has returned true: that wait is also what
  // establishes the happens-before edge from the server thread's last
  // book update to these reads.
  int final_bid_cents() const;
  int final_ask_cents() const;

 private:
  struct Impl;  // asio/beast state, server-thread only

  void run();
  std::string snapshot_json() const;
  std::string delta_json(int price_cents, const char* side,
                         const char* size) const;

  Config config_;
  TlsIdentity identity_;
  Impl* impl_ = nullptr;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> running_{false};

  // The walking book: one bid and one ask level, stepped per delta.
  int bid_cents_ = 40;
  int ask_cents_ = 43;

  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
};

}  // namespace basis::testing
