// tests/PacketFramingTests.cpp
//
// Tests for the UE3 packet/bunch framing codec (src/Network/PacketCodec).
//
// Fixtures are REAL bytes captured from a retail RS2 client <-> official
// dedicated server handshake (D:\RE-Tools\rs2_handshake_capture.pcapng), decoded
// against docs/RS2V_ControlChannel_WireSpec_7258.md. These are byte-exact ground
// truth: if the codec decodes them wrong, a real client will reject our framing.

#include <gtest/gtest.h>

#include "Network/PacketCodec.h"

#include <vector>
#include <cstdint>

using PacketCodec::Decode;

// Frame 19 (C->S): the client's control-channel-opening Hello bunch.
// PacketId 0, no acks, one reliable open bunch on control channel 0,
// ChSequence 1, ChType 1 (control), 16 payload bits (NMT byte + 1).
TEST(PacketFraming, DecodesHelloOpeningBunch) {
    const std::vector<uint8_t> hello = {
        0x00, 0x80, 0x05, 0x20, 0x80, 0x40, 0x00, 0x1d, 0x01, 0x01};

    PacketCodec::Packet pkt = Decode(hello.data(), hello.size());

    ASSERT_TRUE(pkt.ok);
    EXPECT_EQ(pkt.packetId, 0u);
    EXPECT_TRUE(pkt.acks.empty());
    ASSERT_EQ(pkt.bunches.size(), 1u);

    const PacketCodec::Bunch& b = pkt.bunches[0];
    EXPECT_TRUE(b.bControl);
    EXPECT_TRUE(b.bOpen);
    EXPECT_FALSE(b.bClose);
    EXPECT_TRUE(b.bReliable);
    EXPECT_EQ(b.chIndex, 0u);
    EXPECT_EQ(b.chSequence, 1u);
    EXPECT_EQ(b.chType, 1u);
    EXPECT_EQ(b.payloadBits, 16u);

    // First payload byte is the NMT message type: NMT_Hello == 0x00.
    ASSERT_FALSE(b.payload.empty());
    EXPECT_EQ(b.payload[0], 0x00);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
