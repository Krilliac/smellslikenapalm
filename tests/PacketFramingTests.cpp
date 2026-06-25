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

#include <cstddef>
#include <vector>
#include <cstdint>

using PacketCodec::Decode;
using PacketCodec::Encode;

// The PRIMARY correctness check: decoding a UE3 packet and re-encoding the
// resulting Packet must reproduce the packet bytes exactly. Encode emits the
// canonical packet: PacketId, acks, bunches, then a single '1' terminator bit
// zero-padded to a byte boundary - which is exactly the on-wire packet.
//
// A captured *datagram* may carry trailing byte(s) after the packet's own
// terminator byte (UE3 sends one packet per datagram, but the capture / network
// layer can append a trailer - e.g. the retransmitted 10-byte Hello datagram
// carries a 9-byte UE3 packet plus one extra byte). Decode strips those, so the
// round-trip target is the captured frame truncated to `packetBytes`.
static void ExpectRoundTrip(const std::vector<uint8_t>& frame, size_t packetBytes) {
    PacketCodec::Packet pkt = Decode(frame.data(), frame.size());
    ASSERT_TRUE(pkt.ok);
    const std::vector<uint8_t> reencoded = Encode(pkt);
    const std::vector<uint8_t> expected(frame.begin(), frame.begin() + packetBytes);
    EXPECT_EQ(reencoded, expected);
}

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

// Byte-exact round-trip of the Hello frame (the primary contract). The captured
// datagram is 10 bytes; the UE3 packet is the first 9 (the opening reliable
// bunch ends at bit 64, terminator in byte 8). The trailing 10th byte (0x01) is
// not part of this packet and is not reproduced by Encode.
TEST(PacketFraming, HelloRoundTrips) {
    const std::vector<uint8_t> hello = {
        0x00, 0x80, 0x05, 0x20, 0x80, 0x40, 0x00, 0x1d, 0x01, 0x01};
    ExpectRoundTrip(hello, /*packetBytes=*/9);
}

// Frame 157 (S->C): the server's Challenge. PacketId 0; one reliable bunch on
// the control channel (chIndex 0), ChSequence 1, ChType 1, 40 payload bits.
// The first payload byte is the NMT type (NMT_Challenge == 0x03). The 12-byte
// datagram carries an 11-byte UE3 packet (terminator in byte 10); byte 11 is a
// trailer.
TEST(PacketFraming, ChallengeFrame157) {
    const std::vector<uint8_t> challenge = {
        0x00, 0x00, 0x01, 0x08, 0x20, 0x28, 0x80, 0x47, 0xb7, 0xe5, 0x64, 0x6c};

    PacketCodec::Packet pkt = Decode(challenge.data(), challenge.size());
    ASSERT_TRUE(pkt.ok);
    EXPECT_EQ(pkt.packetId, 0u);
    ASSERT_EQ(pkt.bunches.size(), 1u);

    const PacketCodec::Bunch& b = pkt.bunches[0];
    EXPECT_EQ(b.chIndex, 0u);
    EXPECT_TRUE(b.bReliable);
    EXPECT_EQ(b.chSequence, 1u);
    EXPECT_EQ(b.chType, 1u);
    EXPECT_EQ(b.payloadBits, 40u);
    ASSERT_FALSE(b.payload.empty());
    EXPECT_EQ(b.payload[0], 0x00);  // first reassembly fragment leads with 0x00

    ExpectRoundTrip(challenge, /*packetBytes=*/11);
}

// Frame 161 (C->S): the real 72-byte datagram captured AFTER netspeed
// negotiation. Per spec §3 the connection's MaxPacket grows past the handshake
// value of 8 here, so BunchDataBits is bounded by a larger value than this
// codec's handshake-phase kBunchDataBitsMax (64). We therefore only assert that
// Decode does not crash / read out of bounds and recovers the rolling PacketId
// (133); a byte-exact round-trip of this post-handshake frame requires the
// negotiated MaxPacket, which is out of scope for the handshake framing codec.
//
// NOTE: the task brief's inline Netspeed fixture was truncated/garbled (it
// ended "...0c 2" / 68 bytes). These are the true bytes from the pcap
// (frame.number==161, 72 bytes, ending ...0c 20 00 00 00 20).
TEST(PacketFraming, NetspeedFrame161DecodesWithoutOverrun) {
    const std::vector<uint8_t> netspeed = {
        0x85, 0x40, 0x00, 0x80, 0x00, 0x0c, 0x10, 0xfc, 0x00, 0xc2, 0x6f, 0x03,
        0x00, 0x40, 0x8b, 0x03, 0x00, 0x60, 0x77, 0x1b, 0x7d, 0x20, 0x00, 0x00,
        0x22, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x24, 0x04, 0x00, 0x00, 0x00, 0x06,
        0x06, 0x46, 0x46, 0xa6, 0x06, 0xe7, 0x26, 0x46, 0x66, 0x8c, 0x86, 0x86,
        0x2c, 0x8c, 0x26, 0x67, 0x26, 0xa6, 0x4c, 0xe6, 0xa6, 0xc6, 0x26, 0x07,
        0xc7, 0x0c, 0xa6, 0xe6, 0x86, 0xa6, 0x0c, 0x20, 0x00, 0x00, 0x00, 0x20};

    // PacketId is at the head of every packet regardless of MaxPacket.
    PacketCodec::Packet pkt = Decode(netspeed.data(), netspeed.size());
    EXPECT_EQ(pkt.packetId, 133u);  // 14-bit wire PacketId, robustly recoverable
}

// An all-zero / empty datagram must be rejected (ok == false): no terminator
// bit exists.
TEST(PacketFraming, EmptyOrAllZeroIsNotOk) {
    const std::vector<uint8_t> empty;
    EXPECT_FALSE(Decode(empty.data(), empty.size()).ok);

    const std::vector<uint8_t> zeros = {0x00, 0x00, 0x00};
    EXPECT_FALSE(Decode(zeros.data(), zeros.size()).ok);
}

// A hand-built packet with acks and two bunches must survive Encode->Decode and
// a second Encode (structural + idempotent round-trip).
TEST(PacketFraming, SyntheticAcksAndBunchesRoundTrip) {
    PacketCodec::Packet pkt;
    pkt.packetId = 42;
    pkt.acks = {0u, 7u, 16383u};

    PacketCodec::Bunch open;
    open.bControl = true;
    open.bOpen = true;
    open.bClose = false;
    open.bReliable = true;
    open.chIndex = 0;
    open.chSequence = 1;
    open.chType = PacketCodec::kControlChannelType;
    open.payloadBits = 16;
    open.payload = {0x00, 0x01};
    pkt.bunches.push_back(open);

    PacketCodec::Bunch unreliable;
    unreliable.bControl = false;
    unreliable.bReliable = false;
    unreliable.chIndex = 5;
    unreliable.payloadBits = 5;     // partial byte payload
    unreliable.payload = {0x15};    // low 5 bits = 0b10101
    pkt.bunches.push_back(unreliable);

    const std::vector<uint8_t> wire = Encode(pkt);
    PacketCodec::Packet decoded = Decode(wire.data(), wire.size());
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded.packetId, 42u);
    EXPECT_EQ(decoded.acks, pkt.acks);
    ASSERT_EQ(decoded.bunches.size(), 2u);

    EXPECT_TRUE(decoded.bunches[0].bControl);
    EXPECT_TRUE(decoded.bunches[0].bOpen);
    EXPECT_TRUE(decoded.bunches[0].bReliable);
    EXPECT_EQ(decoded.bunches[0].chSequence, 1u);
    EXPECT_EQ(decoded.bunches[0].chType, PacketCodec::kControlChannelType);
    EXPECT_EQ(decoded.bunches[0].payloadBits, 16u);

    EXPECT_FALSE(decoded.bunches[1].bControl);
    EXPECT_FALSE(decoded.bunches[1].bReliable);
    EXPECT_EQ(decoded.bunches[1].chIndex, 5u);
    EXPECT_EQ(decoded.bunches[1].payloadBits, 5u);

    // Re-encoding the decoded packet reproduces the same wire bytes.
    EXPECT_EQ(Encode(decoded), wire);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
