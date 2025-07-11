// tests/ProtocolTests.cpp
// Comprehensive unit tests for protocol serialization, enums, and boundary conditions
//
// Covers:
// 1. PacketType enum <-> tag conversions.
// 2. ProtocolUtils functions: TagToName, NameToTag, validity checks.
// 3. PacketSerializer serialization/deserialization of various packet types.
// 4. Compression flag handling in ProtocolUtils.
// 5. Boundary and error cases: invalid tags, oversized payloads, empty payloads.
// 6. Round-trip tests for all defined PacketTypes.

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include "Protocol/PacketTypes.h"
#include "Protocol/ProtocolUtils.h"
#include "Network/PacketSerializer.h"
#include "Utils/Logger.h"

using ::testing::_;

// Fixture for ProtocolUtils enum conversions
class ProtocolUtilsTest : public ::testing::Test {
protected:
    std::vector<PacketType> allTypes;

    void SetUp() override {
        for (int i = 0; i <= static_cast<int>(PacketType::PT_UNKNOWN); ++i) {
            allTypes.push_back(static_cast<PacketType>(i));
        }
    }
};

TEST_F(ProtocolUtilsTest, TagToName_NameToTag_RoundTrip) {
    for (auto type : allTypes) {
        uint16_t tag = ProtocolUtils::TypeToTag(type);
        PacketType decoded = ProtocolUtils::TagToType(tag);
        EXPECT_EQ(decoded, type);
        std::string name = ProtocolUtils::TagToName(tag);
        PacketType fromName = ProtocolUtils::NameToTag(name);
        EXPECT_EQ(fromName, type);
    }
}

TEST_F(ProtocolUtilsTest, NameToTag_InvalidName_ReturnsUnknown) {
    EXPECT_EQ(ProtocolUtils::NameToTag("INVALID_PACKET"), PacketType::PT_UNKNOWN);
}

TEST_F(ProtocolUtilsTest, TagToName_InvalidTag_ReturnsUnknown) {
    EXPECT_EQ(ProtocolUtils::TagToName(0xFFFF), "PT_UNKNOWN");
    EXPECT_EQ(ProtocolUtils::TagToType(0xFFFF), PacketType::PT_UNKNOWN);
}

// Fixture for PacketSerializer tests
class PacketSerializerTest : public ::testing::Test {
protected:
    PacketSerializer serializer;
};

TEST_F(PacketSerializerTest, SerializeDeserialize_EmptyPayload) {
    PacketType type = PacketType::PT_HEARTBEAT;
    std::vector<uint8_t> empty;
    auto buf = serializer.Serialize(type, empty);
    EXPECT_TRUE(serializer.ValidateHeader(buf.data(), buf.size()));
    Packet packet = serializer.Deserialize(buf);
    EXPECT_EQ(packet.GetType(), type);
    EXPECT_TRUE(packet.GetData().empty());
}

TEST_F(PacketSerializerTest, SerializeDeserialize_OversizedPayload_Throws) {
    size_t max = serializer.GetMaxPacketSize();
    std::vector<uint8_t> data(max + 1, 0xFF);
    EXPECT_THROW(serializer.Serialize(PacketType::PT_ACTOR_REPLICATION, data), std::runtime_error);
}

TEST_F(PacketSerializerTest, CompressionFlag_Preserved) {
    Packet p;
    p.SetType(PacketType::PT_ACTOR_REPLICATION);
    p.SetClientId(123);
    std::vector<uint8_t> data(200, 0xAB);
    p.SetData(data);
    p.SetCompressed(true);
    auto buf = serializer.Serialize(p);
    Packet q = serializer.Deserialize(buf);
    EXPECT_TRUE(q.IsCompressed());
    EXPECT_EQ(q.GetType(), p.GetType());
    EXPECT_EQ(q.RawData(), data);
}

TEST_F(PacketSerializerTest, HeaderLengthMismatch_Detected) {
    auto buf = serializer.Serialize(PacketType::PT_HEARTBEAT, {1,2,3});
    // Corrupt length in header
    uint32_t wrongLen = 1000;
    memcpy(buf.data() + 2, &wrongLen, sizeof(wrongLen));
    EXPECT_THROW(serializer.Deserialize(buf), std::runtime_error);
}

// Test all PacketTypes serialization round-trip
TEST_F(PacketSerializerTest, RoundTrip_AllPacketTypes) {
    for (int i = 0; i <= static_cast<int>(PacketType::PT_UNKNOWN); ++i) {
        PacketType type = static_cast<PacketType>(i);
        std::vector<uint8_t> payload( (i % 4 + 1) * 10, static_cast<uint8_t>(i));
        auto buf = serializer.Serialize(type, payload);
        Packet p = serializer.Deserialize(buf);
        EXPECT_EQ(p.GetType(), type);
        EXPECT_EQ(p.GetData(), payload);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}