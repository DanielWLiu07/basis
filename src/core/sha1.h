#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace basis::core {

// SHA-1 of a single buffer, hex-encoded. Polymarket's order book
// integrity hashes are SHA-1 over a canonical JSON payload
// (feed/polymarket_parser.cpp), which is the only reason this exists:
// SHA-1 is fine for integrity accounting against an honest venue and
// wrong for anything adversarial. Implemented locally because the
// default build has no crypto dependency and one primitive does not
// justify adding OpenSSL to it.
//
// Reference: RFC 3174. Verified against its test vectors in
// tests/test_sha1.cpp.
inline std::string sha1_hex(std::string_view data) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                        0xC3D2E1F0u};

  const auto process_block = [&h](const unsigned char* block) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    const auto rotl = [](std::uint32_t v, int n) {
      return (v << n) | (v >> (32 - n));
    };
    for (int i = 16; i < 80; ++i) {
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f;
      std::uint32_t k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  };

  // Process whole blocks, then the padded tail (length in bits, big
  // endian, after a 0x80 marker and zero fill).
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t remaining = data.size();
  while (remaining >= 64) {
    process_block(bytes);
    bytes += 64;
    remaining -= 64;
  }
  std::array<unsigned char, 128> tail{};
  for (std::size_t i = 0; i < remaining; ++i) tail[i] = bytes[i];
  tail[remaining] = 0x80;
  const std::size_t tail_blocks = (remaining + 1 + 8 > 64) ? 2 : 1;
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[tail_blocks * 64 - 8 + static_cast<std::size_t>(i)] =
        static_cast<unsigned char>(bit_len >> (56 - i * 8));
  }
  process_block(tail.data());
  if (tail_blocks == 2) process_block(tail.data() + 64);

  std::string hex(40, '0');
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 8; ++j) {
      hex[static_cast<std::size_t>(i * 8 + j)] =
          kDigits[(h[i] >> (28 - j * 4)) & 0xF];
    }
  }
  return hex;
}

}  // namespace basis::core
