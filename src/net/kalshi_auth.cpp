#include "net/kalshi_auth.h"

#include <cstdio>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace basis::net {

namespace {

std::string last_openssl_error() {
  const auto code = ERR_get_error();
  char buf[256];
  ERR_error_string_n(code, buf, sizeof(buf));
  return buf;
}

}  // namespace

std::optional<KalshiSigner> KalshiSigner::load(const std::string& pem_path,
                                               std::string* error) {
  std::FILE* file = std::fopen(pem_path.c_str(), "r");
  if (file == nullptr) {
    if (error) *error = "cannot open key file " + pem_path;
    return std::nullopt;
  }
  EVP_PKEY* raw = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
  std::fclose(file);
  if (raw == nullptr) {
    if (error) {
      *error = "cannot parse private key " + pem_path + ": " +
               last_openssl_error();
    }
    return std::nullopt;
  }
  if (EVP_PKEY_base_id(raw) != EVP_PKEY_RSA) {
    EVP_PKEY_free(raw);
    if (error) *error = pem_path + " is not an RSA key";
    return std::nullopt;
  }
  return KalshiSigner(std::shared_ptr<evp_pkey_st>(raw, EVP_PKEY_free));
}

std::optional<std::string> KalshiSigner::sign(std::string_view message) const {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) return std::nullopt;

  // PSS with salt length equal to the digest length is the scheme Kalshi's
  // verifier expects; the default (max salt) fails auth.
  EVP_PKEY_CTX* pkey_ctx = nullptr;
  bool ok =
      EVP_DigestSignInit(ctx, &pkey_ctx, EVP_sha256(), nullptr,
                         pkey_.get()) == 1 &&
      EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING) == 1 &&
      EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, RSA_PSS_SALTLEN_DIGEST) == 1;

  std::size_t sig_len = 0;
  ok = ok && EVP_DigestSign(ctx, nullptr, &sig_len,
                            reinterpret_cast<const unsigned char*>(
                                message.data()),
                            message.size()) == 1;
  std::string signature(sig_len, '\0');
  ok = ok && EVP_DigestSign(ctx,
                            reinterpret_cast<unsigned char*>(signature.data()),
                            &sig_len,
                            reinterpret_cast<const unsigned char*>(
                                message.data()),
                            message.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok) return std::nullopt;
  signature.resize(sig_len);

  // EVP_EncodeBlock writes 4 * ceil(n / 3) base64 bytes plus a NUL.
  std::string encoded(4 * ((sig_len + 2) / 3) + 1, '\0');
  const int written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(encoded.data()),
      reinterpret_cast<const unsigned char*>(signature.data()),
      static_cast<int>(sig_len));
  if (written < 0) return std::nullopt;
  encoded.resize(static_cast<std::size_t>(written));
  return encoded;
}

std::vector<std::pair<std::string, std::string>> KalshiSigner::ws_headers(
    const std::string& key_id, std::string_view path,
    std::int64_t now_ms) const {
  const auto timestamp = std::to_string(now_ms);
  std::string message = timestamp;
  message += "GET";
  message += path;
  const auto signature = sign(message);
  if (!signature) return {};
  return {{"KALSHI-ACCESS-KEY", key_id},
          {"KALSHI-ACCESS-SIGNATURE", *signature},
          {"KALSHI-ACCESS-TIMESTAMP", timestamp}};
}

}  // namespace basis::net
