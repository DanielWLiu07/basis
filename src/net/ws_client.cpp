#include "net/ws_client.h"

#include <algorithm>
#include <chrono>
#include <random>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "core/logger.h"
#include "core/time.h"

namespace basis::net {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// All the per-connection state, built fresh on every (re)connect attempt
// and only ever touched from the IO thread, except the socket shutdown in
// stop() which is what breaks a blocking read.
struct WsClient::Connection {
  asio::io_context ioc;
  ssl::context tls{ssl::context::tls_client};
  websocket::stream<ssl::stream<tcp::socket>> ws{ioc, tls};
};

WsClient::WsClient(WsConfig config) : config_(std::move(config)) {}

WsClient::~WsClient() { stop(); }

void WsClient::set_on_connect(ConnectHandler handler) {
  on_connect_ = std::move(handler);
}

void WsClient::set_on_message(MessageHandler handler) {
  on_message_ = std::move(handler);
}

void WsClient::set_header_provider(HeaderProvider provider) {
  header_provider_ = std::move(provider);
}

void WsClient::start() {
  if (running_.exchange(true)) return;
  io_thread_ = std::thread([this] { run(); });
  if (config_.idle_timeout_ms > 0 || config_.connect_timeout_ms > 0) {
    watchdog_thread_ = std::thread([this] { watchdog(); });
  }
}

// Breaks a read that has gone quiet, so the run loop's reconnect path gets
// a chance to run. Uses the same socket shutdown stop() uses, and for the
// same reason: it is the documented way to wake a thread blocked in recv,
// and it leaves closing the fd to the IO thread that owns it.
//
// The difference from stop() is that running_ stays true, so the run loop
// treats it as a failed connection and reconnects rather than exiting.
void WsClient::watchdog() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!running_.load()) break;
    const std::int64_t last = last_activity_ns_.load();
    if (last == 0) continue;  // between connections

    const std::lock_guard<std::mutex> lock(conn_mutex_);
    // Which budget applies depends on the phase: a connection that has not
    // finished its handshakes is being timed for establishment, an
    // established one for silence.
    const bool established = conn_ != nullptr;
    if (!established && connecting_ == nullptr) continue;
    const std::int64_t budget_ms = established ? config_.idle_timeout_ms
                                               : config_.connect_timeout_ms;
    if (budget_ms <= 0) continue;
    const auto timeout_ns = budget_ms * 1'000'000;
    // Re-check under the lock: the IO thread may have read a frame
    // between the test above and acquiring this.
    if (time::wall_ns() - last_activity_ns_.load() < timeout_ns) continue;
    log::warn("ws " + config_.host + ": " +
              (established ? "no data for " : "handshake stalled ") +
              std::to_string(budget_ms) + "ms; forcing reconnect");
    stalls_.fetch_add(1);
    // Stop the watchdog re-firing on the same silence while the run loop
    // works through its backoff.
    last_activity_ns_.store(time::wall_ns());
    shutdown_active_locked();
  }
}

// shutdown() from another thread is the documented way to wake a socket
// blocked in recv or in a handshake; the IO thread then sees the error and
// destroys the connection (which closes the socket) itself. Deliberately
// not close() here: closing the fd from this thread while the IO thread is
// mid-call races on the fd.
void WsClient::shutdown_active_locked() {
  Connection* target = conn_ != nullptr ? conn_ : connecting_;
  if (target == nullptr) return;
  boost::system::error_code ec;
  target->ws.next_layer().next_layer().shutdown(tcp::socket::shutdown_both, ec);
}

void WsClient::stop() {
  if (!running_.exchange(false)) {
    if (io_thread_.joinable()) io_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    return;
  }
  {
    // Breaks a blocked read, and also a handshake that never completes.
    const std::lock_guard<std::mutex> lock(conn_mutex_);
    shutdown_active_locked();
  }
  if (io_thread_.joinable()) io_thread_.join();
  if (watchdog_thread_.joinable()) watchdog_thread_.join();
}

bool WsClient::send(const std::string& text) {
  const std::lock_guard<std::mutex> lock(conn_mutex_);
  if (conn_ == nullptr) return false;
  boost::system::error_code ec;
  conn_->ws.write(asio::buffer(text), ec);
  return !ec;
}

void WsClient::run() {
  // Backoff jitter only needs to decorrelate reconnect storms; seeding from
  // the wall clock is fine here (this is not the deterministic path).
  std::mt19937 jitter_rng(static_cast<unsigned>(time::wall_ns() & 0xffffffff));
  std::int64_t backoff_ms = config_.initial_backoff_ms;
  bool ever_connected = false;

  while (running_.load()) {
    auto conn = std::make_unique<Connection>();
    boost::system::error_code ec;

    // Trust anchors live on the context's certificate store, which the
    // SSL object shares by reference, so these apply even though the
    // stream already exists.
    conn->tls.set_default_verify_paths(ec);
    if (!config_.trusted_ca_pem.empty()) {
      boost::system::error_code ca_ec;
      conn->tls.add_certificate_authority(
          asio::buffer(config_.trusted_ca_pem), ca_ec);
      if (ca_ec) {
        // A trust anchor that will not load means we would fall back to
        // the system store alone and likely fail closed later; say so
        // rather than silently dropping it.
        log::warn("ws: trusted_ca_pem rejected: " + ca_ec.message());
      }
    }
    // verify_mode and the verify callback must be set on the stream, not
    // the context: OpenSSL copies both into the SSL object when it is
    // created (which already happened when Connection was built), so a
    // later change on the context would silently never take effect and
    // the connection would run unverified. Peer verification alone
    // accepts any valid certificate for any host; pinning the expected
    // hostname is what stops a redirected connection from presenting
    // someone else's perfectly valid certificate.
    conn->ws.next_layer().set_verify_mode(ssl::verify_peer, ec);
    if (!ec) {
      conn->ws.next_layer().set_verify_callback(
          ssl::host_name_verification(config_.host), ec);
    }

    // Resolve + TCP connect.
    tcp::resolver resolver(conn->ioc);
    const auto endpoints = resolver.resolve(config_.host, config_.port, ec);
    if (!ec) {
      asio::connect(conn->ws.next_layer().next_layer(), endpoints, ec);
    }
    if (!ec) {
      const std::lock_guard<std::mutex> lock(conn_mutex_);
      connecting_ = conn.get();
      // Start the idle clock here rather than after the handshakes: a
      // handshake that never finishes is exactly the silence the watchdog
      // exists to break.
      last_activity_ns_.store(time::wall_ns());
    }

    // TLS handshake (SNI first, or the venue rejects the hello).
    if (!ec) {
      if (SSL_set_tlsext_host_name(conn->ws.next_layer().native_handle(),
                                   config_.host.c_str()) != 1) {
        ec = boost::system::error_code(
            static_cast<int>(::ERR_get_error()),
            boost::asio::error::get_ssl_category());
      }
    }
    if (!ec) conn->ws.next_layer().handshake(ssl::stream_base::client, ec);

    // WebSocket handshake.
    //
    // These timeout options are set for the handshake, which is the part
    // of this client that does use them. They do NOT protect the read
    // loop: Beast applies stream_base::timeout to asynchronous operations
    // only, and the loop below reads synchronously. keep_alive_pings is
    // also inert while idle_timeout is `none`, the client-role default.
    // A stalled connection is caught by WsClient::watchdog instead.
    if (!ec) {
      websocket::stream_base::timeout timeout =
          websocket::stream_base::timeout::suggested(beast::role_type::client);
      timeout.keep_alive_pings = true;
      conn->ws.set_option(timeout);
      // Fresh per-connect headers before the static ones: a timestamped
      // signature minted at construction time would be stale by now.
      const auto minted = header_provider_
                              ? header_provider_()
                              : std::vector<std::pair<std::string,
                                                      std::string>>{};
      // The decorator outlives this scope inside the stream object, so it
      // captures by value.
      conn->ws.set_option(websocket::stream_base::decorator(
          [this, minted](websocket::request_type& req) {
            for (const auto& [name, value] : minted) {
              req.set(name, value);
            }
            for (const auto& [name, value] : config_.headers) {
              req.set(name, value);
            }
          }));
      conn->ws.handshake(config_.host, config_.target, ec);
    }

    {
      const std::lock_guard<std::mutex> lock(conn_mutex_);
      connecting_ = nullptr;
      if (!ec) {
        conn_ = conn.get();
        // Restart the clock: the idle budget measures silence on an
        // established connection, not the handshakes that preceded it.
        last_activity_ns_.store(time::wall_ns());
      }
    }

    if (!ec) {
      if (ever_connected) reconnects_.fetch_add(1);
      ever_connected = true;
      backoff_ms = config_.initial_backoff_ms;  // healthy again
      log::info("ws connected: " + config_.host + config_.target);

      if (on_connect_) on_connect_(*this);

      beast::flat_buffer buffer;
      while (running_.load()) {
        buffer.clear();
        conn->ws.read(buffer, ec);
        if (ec) break;
        const auto recv_ns = time::wall_ns();
        last_activity_ns_.store(recv_ns);
        messages_.fetch_add(1);
        bytes_.fetch_add(buffer.size());
        if (on_message_) {
          on_message_(std::string_view(
                          static_cast<const char*>(buffer.data().data()),
                          buffer.size()),
                      recv_ns);
        }
      }

      {
        const std::lock_guard<std::mutex> lock(conn_mutex_);
        conn_ = nullptr;
      }
      last_activity_ns_.store(0);
    }

    if (!running_.load()) break;
    log::warn("ws " + config_.host + ": " +
              (ec ? ec.message() : "connection closed") + "; retry in " +
              std::to_string(backoff_ms) + "ms");

    // Interruptible backoff: stop() must not wait out the timer.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(backoff_ms);
    while (running_.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto jitter = static_cast<std::int64_t>(jitter_rng() % 250);
    backoff_ms = std::min(backoff_ms * 2, config_.max_backoff_ms) + jitter;
  }
}

}  // namespace basis::net
