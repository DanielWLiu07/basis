#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// OpenSSL type, forward-declared so this header stays OpenSSL-free.
struct evp_pkey_st;

namespace basis::net {

// Signs Kalshi API requests. Kalshi authenticates every WebSocket session,
// market data included: an RSA-PSS / SHA-256 signature over
// timestamp_ms + method + path, base64-encoded, sent as three headers on
// the upgrade request (docs/api_integration.md).
//
// The private key comes from a PEM file that must never be committed;
// .gitignore covers secrets/. The signer is cheap to copy (shared key
// handle) and signing is thread-safe: OpenSSL contexts are per-call.
class KalshiSigner {
 public:
  // Reads an RSA private key PEM. Fails (with a reason in *error) rather
  // than degrading: a bad key file must stop a session before it connects.
  static std::optional<KalshiSigner> load(const std::string& pem_path,
                                          std::string* error = nullptr);

  // base64(RSA-PSS-SHA256(message)), salt length = digest length, which is
  // what Kalshi's verifier expects. Empty only on an OpenSSL failure.
  std::optional<std::string> sign(std::string_view message) const;

  // The three auth headers for a WebSocket upgrade at time now_ms:
  //   KALSHI-ACCESS-KEY, KALSHI-ACCESS-SIGNATURE, KALSHI-ACCESS-TIMESTAMP
  // signing timestamp_ms + "GET" + path. Empty on signing failure.
  std::vector<std::pair<std::string, std::string>> ws_headers(
      const std::string& key_id, std::string_view path,
      std::int64_t now_ms) const;

 private:
  explicit KalshiSigner(std::shared_ptr<evp_pkey_st> pkey)
      : pkey_(std::move(pkey)) {}

  std::shared_ptr<evp_pkey_st> pkey_;
};

}  // namespace basis::net
