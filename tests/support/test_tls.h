#pragma once

#include <string>

namespace basis::testing {

// A throwaway TLS identity for the fault-injection server: private key
// and self-signed certificate, generated fresh per test run so no key
// material ever lives in the repo. The certificate carries subject
// alternative names for 127.0.0.1 and localhost, which is what lets the
// client keep full peer and hostname verification on while talking to
// the local server.
struct TlsIdentity {
  std::string cert_pem;
  std::string key_pem;
};

TlsIdentity make_self_signed_identity();

}  // namespace basis::testing
