#include "support/flaky_ws_server.h"

#include <chrono>
#include <cstdio>
#include <exception>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace basis::testing {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct FlakyWsServer::Impl {
  asio::io_context ioc;
  ssl::context tls{ssl::context::tls_server};
  tcp::acceptor acceptor{ioc};
};

FlakyWsServer::FlakyWsServer(Config config, TlsIdentity identity)
    : config_(std::move(config)),
      identity_(std::move(identity)),
      impl_(new Impl) {
  impl_->tls.use_certificate_chain(asio::buffer(identity_.cert_pem));
  impl_->tls.use_private_key(asio::buffer(identity_.key_pem),
                             ssl::context::pem);
  // Bind in the constructor so port() is valid before start().
  const tcp::endpoint local(asio::ip::make_address("127.0.0.1"), 0);
  impl_->acceptor.open(local.protocol());
  impl_->acceptor.bind(local);
  impl_->acceptor.listen();
  port_ = impl_->acceptor.local_endpoint().port();
}

FlakyWsServer::~FlakyWsServer() {
  stop();
  delete impl_;
}

void FlakyWsServer::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void FlakyWsServer::stop() {
  if (running_.exchange(false)) {
    boost::system::error_code ec;
    impl_->acceptor.close(ec);  // breaks a blocking accept
  }
  if (thread_.joinable()) thread_.join();
}

bool FlakyWsServer::wait_finished(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return finished_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return finished_; });
}

int FlakyWsServer::final_bid_cents() const { return bid_cents_; }
int FlakyWsServer::final_ask_cents() const { return ask_cents_; }

namespace {

// Cents to the venue's decimal probability string ("0.43").
std::string price_string(int cents) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d.%02d", cents / 100, cents % 100);
  return buf;
}

}  // namespace

std::string FlakyWsServer::snapshot_json() const {
  std::string json = R"({"event_type":"book","asset_id":")";
  json += config_.asset_id;
  json += R"(","bids":[{"price":")" + price_string(bid_cents_) +
          R"(","size":"100"}],)";
  json += R"("asks":[{"price":")" + price_string(ask_cents_) +
          R"(","size":"100"}]})";
  return json;
}

std::string FlakyWsServer::delta_json(int price_cents, const char* side,
                                      const char* size) const {
  std::string json = R"({"asset_id":")";
  json += config_.asset_id;
  json += R"(","price":")" + price_string(price_cents);
  json += R"(","size":")";
  json += size;
  json += R"(","side":")";
  json += side;
  json += R"("})";
  return json;
}

void FlakyWsServer::run() {
  int step = 0;
  for (int c = 0; c < config_.connections && running_.load(); ++c) {
    try {
      tcp::socket socket(impl_->ioc);
      impl_->acceptor.accept(socket);

      websocket::stream<ssl::stream<tcp::socket>> ws(std::move(socket),
                                                     impl_->tls);
      ws.next_layer().handshake(ssl::stream_base::server);
      ws.accept();

      beast::flat_buffer buffer;
      ws.read(buffer);  // the subscribe message; contents are irrelevant

      // The recovery contract: every (re)subscription gets a snapshot.
      ws.write(asio::buffer(snapshot_json()));

      for (int d = 0; d < config_.deltas_per_connection; ++d, ++step) {
        const int old_bid = bid_cents_;
        const int old_ask = ask_cents_;
        bid_cents_ = 30 + (step * 3) % 25;
        ask_cents_ = bid_cents_ + 3;
        // One price_change message moving both sides: remove the old
        // levels, set the new ones.
        std::string json = R"({"event_type":"price_change","asset_id":")" +
                           config_.asset_id + R"(","price_changes":[)";
        json += delta_json(old_bid, "BUY", "0") + ",";
        json += delta_json(bid_cents_, "BUY", "100") + ",";
        json += delta_json(old_ask, "SELL", "0") + ",";
        json += delta_json(ask_cents_, "SELL", "100") + "]}";
        ws.write(asio::buffer(json));
      }

      if (c + 1 < config_.connections) {
        // The injected fault: a hard TCP close mid-subscription, no
        // WebSocket close frame, exactly what a dying LB looks like.
        boost::system::error_code ec;
        ws.next_layer().next_layer().close(ec);
      } else {
        boost::system::error_code ec;
        ws.close(websocket::close_code::normal, ec);
      }
    } catch (const std::exception&) {
      if (!running_.load()) break;
      // A failed connection does not end the script; the client is
      // expected to come back.
    }
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
  }
  finished_cv_.notify_all();
}

}  // namespace basis::testing
