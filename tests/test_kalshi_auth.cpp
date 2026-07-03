// Compiled only when BASIS_ENABLE_NET is on (see tests/CMakeLists.txt).
// Signing is verified fully offline: a throwaway RSA key generated here
// signs the way KalshiSigner will in production, and OpenSSL verifies the
// signature against the public half with the exact PSS parameters Kalshi
// documents. No network, no real credentials.

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "net/kalshi_auth.h"

namespace {

using basis::net::KalshiSigner;

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

PkeyPtr generate_rsa_key() {
  return PkeyPtr(EVP_RSA_gen(2048), EVP_PKEY_free);
}

std::string write_private_pem(EVP_PKEY* pkey, const std::string& name) {
  const auto path =
      (std::filesystem::temp_directory_path() / name).string();
  std::FILE* file = std::fopen(path.c_str(), "w");
  PEM_write_PrivateKey(file, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  std::fclose(file);
  return path;
}

// Verifies base64-decoded RSA-PSS/SHA-256 with salt length = digest
// length, the parameters Kalshi's verifier uses.
bool verify_signature(EVP_PKEY* pkey, const std::string& message,
                      const std::string& signature_b64) {
  std::string signature(3 * (signature_b64.size() / 4), '\0');
  const int decoded = EVP_DecodeBlock(
      reinterpret_cast<unsigned char*>(signature.data()),
      reinterpret_cast<const unsigned char*>(signature_b64.data()),
      static_cast<int>(signature_b64.size()));
  if (decoded < 0) return false;
  // EVP_DecodeBlock cannot know about padding; a 2048-bit RSA signature is
  // exactly 256 bytes.
  signature.resize(256);

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_PKEY_CTX* pkey_ctx = nullptr;
  bool ok =
      EVP_DigestVerifyInit(ctx, &pkey_ctx, EVP_sha256(), nullptr, pkey) == 1 &&
      EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING) == 1 &&
      EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, RSA_PSS_SALTLEN_DIGEST) == 1;
  ok = ok && EVP_DigestVerify(
                 ctx,
                 reinterpret_cast<const unsigned char*>(signature.data()),
                 signature.size(),
                 reinterpret_cast<const unsigned char*>(message.data()),
                 message.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

TEST(KalshiSigner, SignatureVerifiesAgainstThePublicKey) {
  const auto pkey = generate_rsa_key();
  ASSERT_NE(pkey, nullptr);
  const auto pem = write_private_pem(pkey.get(), "basis_test_kalshi.pem");

  std::string error;
  auto signer = KalshiSigner::load(pem, &error);
  ASSERT_TRUE(signer.has_value()) << error;
  std::filesystem::remove(pem);

  const std::string message = "1767225600000GET/trade-api/ws/v2";
  const auto signature = signer->sign(message);
  ASSERT_TRUE(signature.has_value());
  EXPECT_TRUE(verify_signature(pkey.get(), message, *signature));
  // PSS is randomized, so a second signature differs but still verifies.
  const auto second = signer->sign(message);
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*signature, *second);
  EXPECT_TRUE(verify_signature(pkey.get(), message, *second));
  // A tampered message must not verify.
  EXPECT_FALSE(verify_signature(pkey.get(),
                                "1767225600001GET/trade-api/ws/v2",
                                *signature));
}

TEST(KalshiSigner, WsHeadersCarryKeySignatureAndTimestamp) {
  const auto pkey = generate_rsa_key();
  const auto pem = write_private_pem(pkey.get(), "basis_test_kalshi2.pem");
  auto signer = KalshiSigner::load(pem);
  ASSERT_TRUE(signer.has_value());
  std::filesystem::remove(pem);

  const auto headers =
      signer->ws_headers("my-key-id", "/trade-api/ws/v2", 1767225600000);
  ASSERT_EQ(headers.size(), 3u);
  EXPECT_EQ(headers[0].first, "KALSHI-ACCESS-KEY");
  EXPECT_EQ(headers[0].second, "my-key-id");
  EXPECT_EQ(headers[1].first, "KALSHI-ACCESS-SIGNATURE");
  EXPECT_TRUE(verify_signature(pkey.get(), "1767225600000GET/trade-api/ws/v2",
                               headers[1].second));
  EXPECT_EQ(headers[2].first, "KALSHI-ACCESS-TIMESTAMP");
  EXPECT_EQ(headers[2].second, "1767225600000");
}

TEST(KalshiSigner, LoadFailsLoudlyOnMissingOrGarbageKey) {
  std::string error;
  EXPECT_FALSE(KalshiSigner::load("/nonexistent/key.pem", &error).has_value());
  EXPECT_NE(error.find("cannot open"), std::string::npos);

  const auto path =
      (std::filesystem::temp_directory_path() / "basis_test_garbage.pem")
          .string();
  std::FILE* file = std::fopen(path.c_str(), "w");
  std::fputs("not a pem file", file);
  std::fclose(file);
  EXPECT_FALSE(KalshiSigner::load(path, &error).has_value());
  EXPECT_NE(error.find("cannot parse"), std::string::npos);
  std::filesystem::remove(path);
}

}  // namespace
