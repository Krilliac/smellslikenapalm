// tests/ProtocolUtilsTests.cpp
//
// Native unit tests for src/Protocol/ProtocolUtils.{h,cpp}: tag split/join,
// hex and base64 codecs, the XOR frame checksum, and PacketType<->tag mapping.
// Asserts published-style invariants and round-trips rather than magic values.

#include "TestFramework.h"

#include "Protocol/ProtocolUtils.h"
#include "Protocol/PacketTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
}  // namespace

TEST(ProtocolUtils, HexRoundTrip) {
    std::vector<uint8_t> data = {0x00, 0x01, 0x7f, 0x80, 0xff, 0xab, 0xcd};
    std::string hex = ProtocolUtils::ToHexString(data);
    EXPECT_EQ(hex.size(), data.size() * 2);
    auto back = ProtocolUtils::FromHexString(hex);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, data);
}

TEST(ProtocolUtils, FromHexStringRejectsBadInput) {
    // Odd length is not decodable.
    EXPECT_FALSE(ProtocolUtils::FromHexString("abc").has_value());
    // Non-hex characters are not decodable.
    EXPECT_FALSE(ProtocolUtils::FromHexString("zz").has_value());
}

TEST(ProtocolUtils, Base64RoundTrip) {
    for (const std::string& s : std::vector<std::string>{
             "x", "payload-123", std::string("\x01\x02\x00\xfe", 4)}) {
        auto enc = ProtocolUtils::Base64Encode(Bytes(s));
        auto dec = ProtocolUtils::Base64Decode(enc);
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(*dec, Bytes(s));
    }
}

TEST(ProtocolUtils, ChecksumIsXorOfBytes) {
    std::vector<uint8_t> p = {0x12, 0x34, 0x56};
    EXPECT_EQ(ProtocolUtils::ComputeChecksum(p), uint8_t(0x12 ^ 0x34 ^ 0x56));
    EXPECT_EQ(ProtocolUtils::ComputeChecksum({}), uint8_t(0));
}

TEST(ProtocolUtils, VerifyAndStripChecksum) {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<uint8_t> framed = data;
    framed.push_back(ProtocolUtils::ComputeChecksum(data));  // append checksum

    ASSERT_TRUE(ProtocolUtils::VerifyAndStripChecksum(framed));
    EXPECT_EQ(framed, data);  // checksum byte stripped, payload intact

    // Corrupted checksum is rejected.
    std::vector<uint8_t> bad = data;
    bad.push_back(0x00);  // wrong checksum (unless data XOR happens to be 0)
    if (ProtocolUtils::ComputeChecksum(data) != 0x00) {
        EXPECT_FALSE(ProtocolUtils::VerifyAndStripChecksum(bad));
    }

    // Empty payload cannot be verified.
    std::vector<uint8_t> empty;
    EXPECT_FALSE(ProtocolUtils::VerifyAndStripChecksum(empty));
}

TEST(ProtocolUtils, TagJoinSplitRoundTrip) {
    auto joined = ProtocolUtils::JoinTag("net", "heartbeat");
    auto parts = ProtocolUtils::SplitTag(joined);
    EXPECT_EQ(parts.first, "net");
    EXPECT_EQ(parts.second, "heartbeat");
}

TEST(ProtocolUtils, PacketTypeTagRoundTrip) {
    // For every type that has a textual tag, TagToType must invert TypeToTag.
    const PacketType types[] = {
        PacketType::PT_HEARTBEAT,
        PacketType::PT_CHAT_MESSAGE,
        PacketType::PT_PLAYER_MOVE,
        PacketType::PT_ACTOR_REPLICATION,
    };
    for (PacketType t : types) {
        std::string tag = ProtocolUtils::TypeToTag(t);
        EXPECT_FALSE(tag.empty()) << "no tag for type " << static_cast<int>(t);
        if (!tag.empty()) {
            EXPECT_EQ(ProtocolUtils::TagToType(tag), t) << "tag: " << tag;
        }
    }
    // An unknown tag maps back to the invalid sentinel.
    EXPECT_EQ(ProtocolUtils::TagToType("definitely.not.a.real.tag"),
              PacketType::PT_INVALID);
}

RS2V_TEST_MAIN()
