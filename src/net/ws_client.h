#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace basis::net {

struct WsConfig {
  std::string host;    // e.g. ws-subscriptions-clob.polymarket.com
  std::string port = "443";
  std::string target;  // e.g. /ws/market
  // Static HTTP headers for the upgrade request. Headers that must be
  // minted per connection (timestamped signatures) go through
  // set_header_provider instead.
  std::vector<std::pair<std::string, std::string>> headers;
  std::int64_t initial_backoff_ms = 500;
  std::int64_t max_backoff_ms = 30'000;
};

// A TLS WebSocket client on its own IO thread. It connects, hands the
// connection to on_connect (subscribe messages are sent there), then reads
// until the peer drops or stop() is called. Any failure reconnects with
// exponential backoff plus jitter; the connect handler runs again after
// every reconnect, so subscriptions survive drops.
//
// Threading contract: on_connect and on_message run on the IO thread.
// send() is only valid from inside on_connect or on_message: both run on
// the IO thread with no read outstanding, so a synchronous write cannot
// race the read loop. stop() is safe from any thread and joins the IO
// thread.
class WsClient {
 public:
  using MessageHandler =
      std::function<void(std::string_view payload, std::int64_t recv_ns)>;
  using ConnectHandler = std::function<void(WsClient&)>;
  // Minted fresh before every handshake, on the IO thread. Kalshi signs a
  // timestamp into its auth headers, so reconnect must re-sign; a stale
  // header set from construction time would be rejected.
  using HeaderProvider =
      std::function<std::vector<std::pair<std::string, std::string>>()>;

  explicit WsClient(WsConfig config);
  ~WsClient();

  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  void set_on_connect(ConnectHandler handler);
  void set_on_message(MessageHandler handler);
  void set_header_provider(HeaderProvider provider);

  void start();
  void stop();

  // Sends a text frame. Only valid from inside on_connect; returns false
  // if the write fails (the read loop will then reconnect).
  bool send(const std::string& text);

  std::uint64_t messages() const { return messages_.load(); }
  std::uint64_t reconnects() const { return reconnects_.load(); }
  std::uint64_t bytes() const { return bytes_.load(); }

 private:
  struct Connection;  // beast/asio state, IO-thread only

  void run();

  WsConfig config_;
  ConnectHandler on_connect_;
  MessageHandler on_message_;
  HeaderProvider header_provider_;

  std::thread io_thread_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> messages_{0};
  std::atomic<std::uint64_t> reconnects_{0};
  std::atomic<std::uint64_t> bytes_{0};

  // Owned by the IO thread; stop() pokes it (under lock) to break a
  // blocking read.
  std::mutex conn_mutex_;
  Connection* conn_ = nullptr;
};

}  // namespace basis::net
