#include "support/test_tls.h"

#include <stdexcept>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace basis::testing {

namespace {

std::string bio_to_string(BIO* bio) {
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  return std::string(data, static_cast<std::size_t>(len));
}

}  // namespace

TlsIdentity make_self_signed_identity() {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  X509* cert = X509_new();
  if (pkey == nullptr || cert == nullptr) {
    throw std::runtime_error("test tls: keygen failed");
  }

  X509_set_version(cert, X509_VERSION_3);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
  X509_gmtime_adj(X509_getm_notAfter(cert), 24 * 3600);
  X509_set_pubkey(cert, pkey);

  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("basis test server"), -1, -1, 0);
  X509_set_issuer_name(cert, name);  // self-signed: issuer = subject

  // Hostname verification checks subject alternative names, so the local
  // addresses go there, not in the CN.
  X509_EXTENSION* san = X509V3_EXT_conf_nid(
      nullptr, nullptr, NID_subject_alt_name,
      "IP:127.0.0.1,DNS:localhost");
  if (san == nullptr || X509_add_ext(cert, san, -1) != 1 ||
      X509_sign(cert, pkey, EVP_sha256()) == 0) {
    throw std::runtime_error("test tls: certificate build failed");
  }
  X509_EXTENSION_free(san);

  BIO* cert_bio = BIO_new(BIO_s_mem());
  BIO* key_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(cert_bio, cert);
  PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr,
                           nullptr);
  TlsIdentity identity{bio_to_string(cert_bio), bio_to_string(key_bio)};

  BIO_free(cert_bio);
  BIO_free(key_bio);
  X509_free(cert);
  EVP_PKEY_free(pkey);
  return identity;
}

}  // namespace basis::testing
