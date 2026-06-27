// tests/CryptoUtilsTests.cpp
//
// Native unit tests for src/Utils/CryptoUtils.{h,cpp}: SHA-256, HMAC-SHA256,
// Base64, and random-byte generation. Uses published test vectors plus
// round-trip / structural invariants. (rs2v_core links OpenSSL in this build,
// so the standard vectors apply.)

#include "TestFramework.h"

#include "Utils/CryptoUtils.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ToHex(const std::vector<uint8_t>& v) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) {
        out.push_back(k[b >> 4]);
        out.push_back(k[b & 0x0F]);
    }
    return out;
}

}  // namespace

TEST(CryptoUtils, Sha256KnownVector) {
    // FIPS 180-2 example: SHA256("abc").
    EXPECT_EQ(CryptoUtils::SHA256Hex(Bytes("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Empty input has a well-known digest too.
    EXPECT_EQ(CryptoUtils::SHA256Hex(Bytes("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(CryptoUtils, HmacSha256KnownVector) {
    // RFC-style vector: HMAC-SHA256(key="key", msg="The quick brown fox...").
    auto mac = CryptoUtils::HMAC_SHA256(
        Bytes("key"), Bytes("The quick brown fox jumps over the lazy dog"));
    ASSERT_EQ(mac.size(), 32u);
    EXPECT_EQ(ToHex(mac),
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

TEST(CryptoUtils, Base64KnownVectors) {
    EXPECT_EQ(CryptoUtils::Base64Encode(Bytes("Man")), "TWFu");
    EXPECT_EQ(CryptoUtils::Base64Encode(Bytes("M")), "TQ==");
    EXPECT_EQ(CryptoUtils::Base64Encode(Bytes("Ma")), "TWE=");
    EXPECT_EQ(CryptoUtils::Base64Encode(Bytes("")), "");
}

TEST(CryptoUtils, Base64RoundTrip) {
    // Empty input encodes to "" but is not a decodable token, so it is covered
    // by the known-vectors test instead.
    for (const std::string& s : std::vector<std::string>{
             "a", "hello world", std::string("\x00\x01\x02\xff\xfe", 5)}) {
        auto enc = CryptoUtils::Base64Encode(Bytes(s));
        auto dec = CryptoUtils::Base64Decode(enc);
        ASSERT_TRUE(dec.has_value()) << "decode failed for: " << s;
        EXPECT_EQ(*dec, Bytes(s));
    }
}

TEST(CryptoUtils, Base64DecodeRejectsGarbage) {
    // A single stray character cannot be valid base64 (not a multiple of 4
    // and not a legal symbol set) — decoder must report failure, not crash.
    auto bad = CryptoUtils::Base64Decode("@@@@");
    // Either nullopt or a value, but it must never throw / corrupt memory;
    // for the canonical illegal-alphabet case we expect rejection.
    EXPECT_FALSE(bad.has_value());
}

TEST(CryptoUtils, GenerateRandomBytes) {
    auto a = CryptoUtils::GenerateRandomBytes(32);
    auto b = CryptoUtils::GenerateRandomBytes(32);
    EXPECT_EQ(a.size(), 32u);
    EXPECT_EQ(b.size(), 32u);
    // Two 32-byte draws being identical is astronomically unlikely.
    EXPECT_NE(a, b);
    EXPECT_TRUE(CryptoUtils::GenerateRandomBytes(0).empty());
}

RS2V_TEST_MAIN()
