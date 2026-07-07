#include <gtest/gtest.h>

#include <string>

#include "core/sha1.h"

using basis::core::sha1_hex;

// RFC 3174 test vectors plus the padding boundary cases (55 and 56 byte
// inputs straddle the point where the length field forces a second
// block).
TEST(Sha1, MatchesTheReferenceVectors) {
  EXPECT_EQ(sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1, PaddingBoundaries) {
  EXPECT_EQ(sha1_hex(std::string(55, 'a')),
            "c1c8bbdc22796e28c0e15163d20899b65621d65a");
  EXPECT_EQ(sha1_hex(std::string(56, 'a')),
            "c2db330f6083854c99d4b5bfb6e8f29f201be699");
  EXPECT_EQ(sha1_hex(std::string(64, 'a')),
            "0098ba824b5c16427bd7a1122a5a442a25ec644d");
  EXPECT_EQ(sha1_hex(std::string(1000, 'a')),
            "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}
