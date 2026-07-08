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
}

void WsClient::stop() {
  if (!running_.exchange(false)) {
    if (io_thread_.joinable()) io_thread_.join();
    return;
  }
  {
    // Break a blocked read: shutdown() from another thread is the
    // documented way to wake a socket blocked in recv, and the IO thread
    // then sees the error and destroys the connection (which closes the
    // socket) itself. Deliberately not close() here: closing the fd from
    // this thread while the IO thread is mid-read races on the fd.
    const std::lock_guard<std::mutex> lock(conn_mutex_);
    if (conn_ != nullptr) {
      boost::system::error_code ec;
      conn_->ws.next_layer().next_layer().shutdown(tcp::socket::shutdown_both,
                                                   ec);
    }
  }
  if (io_thread_.joinable()) io_thread_.join();
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

    // WebSocket handshake. Built-in keep-alive pings hold idle
    // subscriptions open without a hand-rolled heartbeat.
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

    if (!ec) {
      {
        const std::lock_guard<std::mutex> lock(conn_mutex_);
        conn_ = conn.get();
      }
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
