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
  // Extra trust anchor in PEM form, for endpoints whose certificate the
  // system store does not know (the fault-injection test server). The
  // system trust store stays active either way, and peer plus hostname
  // verification are always on; there is deliberately no insecure mode.
  std::string trusted_ca_pem;
  std::int64_t initial_backoff_ms = 500;
  std::int64_t max_backoff_ms = 30'000;

  // Force a reconnect when no frame has arrived for this long. 0 disables.
  //
  // This exists because a dropped network does not necessarily produce a
  // read error. A half-open TCP connection - the peer or a middlebox gone
  // without a FIN or RST - leaves a blocking read parked forever, and the
  // reconnect path below is driven entirely by read errors, so it never
  // runs. Observed in the field: a 45 minute capture where Coinbase went
  // silent 19 minutes in and stayed silent for the remaining 25, with
  // zero reconnects logged, while Binance (whose server does close
  // connections) recovered twice over the same outage.
  //
  // Beast's stream_base::timeout does not cover this. Its settings apply
  // to asynchronous operations only, and this client reads synchronously;
  // keep_alive_pings additionally has no effect while idle_timeout is
  // `none`, which is the client-role default. So the guard has to be
  // external, and it is: a watchdog thread that breaks the read the same
  // way stop() does.
  //
  // 30s is well above any real inter-message gap on the venues here (the
  // quietest, Coinbase level2_batch, batches on a 50 ms timer) and well
  // below the point where a stalled feed stops being obvious.
  std::int64_t idle_timeout_ms = 30'000;

  // The same watchdog, applied to connection establishment: TLS and
  // WebSocket handshakes here are synchronous and have no timeout of their
  // own, so a peer that accepts the TCP connection and then says nothing
  // parks the IO thread forever.
  //
  // This is a separate budget from idle_timeout_ms on purpose. Setting up
  // a connection costs a round trip or several, so charging it against the
  // steady-state data timeout means a short idle timeout kills every
  // attempt before it can finish and the client never makes progress -
  // a livelock that is easy to create by accident and confusing to read,
  // since the log fills with successful-looking reconnects. 0 disables.
  std::int64_t connect_timeout_ms = 15'000;
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
// race the read loop. stop() must be called from a single controlling
// thread (typically the one that called start), not concurrently with
// itself; it joins the IO thread. stop() interrupts a blocked read
// promptly, but a connection still being established (a slow or dead
// endpoint mid-connect) is only torn down once that attempt returns or
// times out; cancelling a synchronous connect safely would require the
// async-with-deadline path, which this client does not use.
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
  // Reconnects forced by the idle watchdog rather than by a read error.
  // Reported separately because they mean something different: the
  // connection did not fail, it went quiet, and only the watchdog noticed.
  std::uint64_t stalls() const { return stalls_.load(); }

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
  std::atomic<std::uint64_t> stalls_{0};

  // Wall-clock nanoseconds of the last frame read, or of the connect that
  // has not yet produced one. Written by the IO thread, read by the
  // watchdog.
  std::atomic<std::int64_t> last_activity_ns_{0};
  std::thread watchdog_thread_;
  void watchdog();

  // Both owned by the IO thread; stop() and the watchdog poke them (under
  // lock) to break a blocking call.
  //
  // conn_ is published once the connection is fully established, and is
  // what send() writes to. connecting_ is published earlier, as soon as
  // the TCP connect returns, and exists only so a wedged handshake can be
  // broken: the TLS and WebSocket handshakes here are synchronous and have
  // no timeout, so a peer that accepts the connection and then says
  // nothing used to park the IO thread forever, taking stop() down with it
  // (it joins that thread). send() deliberately still uses conn_ alone -
  // writing to a stream mid-handshake would corrupt it.
  std::mutex conn_mutex_;
  Connection* conn_ = nullptr;
  Connection* connecting_ = nullptr;
  // Breaks whichever socket is live. Call with conn_mutex_ held.
  void shutdown_active_locked();
};

}  // namespace basis::net
