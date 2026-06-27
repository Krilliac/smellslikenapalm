// tests/PacketAssemblerTests.cpp
//
// Tests for the OUTBOUND control-channel packet assembler
// (src/Network/PacketAssembler). These exercise the STRUCT-level contract
// (PacketCodec::Packet / Bunch) directly — they do NOT depend on
// PacketCodec::Encode producing real bytes (its implementation is being written
// in parallel). Fragment payloads are reassembled with BitReader to prove the
// bunch bit stream reproduces the original message payload bit-for-bit.
//
// Wire rules under test (docs/RS2V_ControlChannel_WireSpec_7258.md §2/§3/§5):
//   * a control bunch carries <= kBunchDataBitsMax-1 (63) data bits;
//   * reliable control bunches use ChIndex 0, ChType 1, consecutive ChSequence
//     starting at 1;
//   * the SERVER never opens the control channel - the CLIENT opens ch0 (its
//     HandshakeStart is bOpen=1); every server ch0 bunch is bControl=0, bOpen=0
//     (verified against the official server; opening it stalled the real client);
//   * the server must ack received PacketIds.

#include "TestFramework.h"

#include "Network/PacketAssembler.h"
#include "Network/PacketCodec.h"
#include "Network/BitReader.h"
#include "Network/NetMessages.h"

#include <cstdint>
#include <vector>

using PacketCodec::Bunch;
using PacketCodec::Packet;
using PacketCodec::PacketAssembler;

namespace {

// Reassemble the bunch payload bit streams (in iteration order) into a flat list
// of bits (LSB-first), reading exactly payloadBits from each bunch.
std::vector<bool> GatherPayloadBits(const std::vector<Bunch>& bunches) {
    std::vector<bool> bits;
    for (const Bunch& b : bunches) {
        BitReader r(b.payload);
        for (uint32_t i = 0; i < b.payloadBits; ++i) {
            bits.push_back(r.ReadBit());
        }
        EXPECT_FALSE(r.IsOverflowed());
    }
    return bits;
}

// Expand a byte buffer into its LSB-first bit sequence (8 bits/byte).
std::vector<bool> BytesToBits(const std::vector<uint8_t>& bytes) {
    std::vector<bool> bits;
    for (uint8_t byte : bytes) {
        for (int i = 0; i < 8; ++i) {
            bits.push_back((byte >> i) & 1u);
        }
    }
    return bits;
}

// Collect every bunch across a list of packets, in send order.
std::vector<Bunch> AllBunches(const std::vector<Packet>& packets) {
    std::vector<Bunch> out;
    for (const Packet& p : packets) {
        for (const Bunch& b : p.bunches) {
            out.push_back(b);
        }
    }
    return out;
}

} // namespace

// A short payload (2 bytes) -> exactly 1 packet, 1 bunch on the (already-open,
// client-opened) control channel: bControl=0, bOpen=0.
TEST(PacketAssembler, ShortPayloadSingleBunch) {
    PacketAssembler asm_;
    const std::vector<uint8_t> payload = {NMTByte(NMT::Hello), 0x01}; // 16 bits

    std::vector<Packet> packets = asm_.BuildControlMessagePackets(payload);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].packetId, 0u);
    ASSERT_EQ(packets[0].bunches.size(), 1u);

    const Bunch& b = packets[0].bunches[0];
    EXPECT_FALSE(b.bControl);  // server never opens ch0
    EXPECT_FALSE(b.bOpen);
    EXPECT_FALSE(b.bClose);
    EXPECT_TRUE(b.bReliable);
    EXPECT_EQ(b.chIndex, 0u);
    EXPECT_EQ(b.chType, PacketCodec::kControlChannelType); // 1
    EXPECT_EQ(b.chSequence, 1u);
    EXPECT_EQ(b.payloadBits, 16u);

    // The bunch payload bits reassemble back to the original payload.
    EXPECT_EQ(GatherPayloadBits(packets[0].bunches), BytesToBits(payload));
}

// A payload whose bit count exceeds 63 -> multiple bunches, consecutive
// ChSequence, each <=63 bits, reassembling to the original payload bit-for-bit.
// No bunch opens the channel (the server never opens ch0).
TEST(PacketAssembler, LongPayloadFragmentsAcrossBunches) {
    PacketAssembler asm_;
    // 16 bytes = 128 bits > 63 => needs >=3 bunches (63 + 63 + 2).
    std::vector<uint8_t> payload;
    for (int i = 0; i < 16; ++i) {
        payload.push_back(static_cast<uint8_t>(0xA0 + i));
    }

    std::vector<Packet> packets = asm_.BuildControlMessagePackets(payload);

    std::vector<Bunch> bunches = AllBunches(packets);
    ASSERT_GE(bunches.size(), 2u);

    // No bunch opens the channel (server never opens ch0).
    for (size_t i = 0; i < bunches.size(); ++i) {
        EXPECT_FALSE(bunches[i].bControl) << "bunch " << i;
        EXPECT_FALSE(bunches[i].bOpen) << "bunch " << i;
    }

    // Every bunch is a reliable control bunch with the right channel attrs and a
    // legal fragment size, with consecutive ChSequence starting at 1.
    uint32_t totalBits = 0;
    for (size_t i = 0; i < bunches.size(); ++i) {
        const Bunch& b = bunches[i];
        EXPECT_TRUE(b.bReliable) << "bunch " << i;
        EXPECT_FALSE(b.bClose) << "bunch " << i;
        EXPECT_EQ(b.chIndex, 0u) << "bunch " << i;
        EXPECT_EQ(b.chType, PacketCodec::kControlChannelType) << "bunch " << i;
        EXPECT_EQ(b.chSequence, static_cast<uint32_t>(i + 1)) << "bunch " << i;
        EXPECT_LE(b.payloadBits, PacketCodec::kBunchDataBitsMax - 1) << "bunch " << i;
        EXPECT_GT(b.payloadBits, 0u) << "bunch " << i;
        totalBits += b.payloadBits;
    }
    EXPECT_EQ(totalBits, payload.size() * 8u);

    // Concatenating all bunch payload bits in ChSequence order reproduces the
    // original payload bit-for-bit.
    EXPECT_EQ(GatherPayloadBits(bunches), BytesToBits(payload));
}

// Consecutive BuildControlMessagePackets calls: neither opens the channel and
// ChSequence continues incrementing across messages.
TEST(PacketAssembler, ChSequenceContinuesAcrossMessages) {
    PacketAssembler asm_;
    const std::vector<uint8_t> first = {NMTByte(NMT::Challenge), 0x02};
    const std::vector<uint8_t> second = {NMTByte(NMT::Welcome), 0x03};

    std::vector<Packet> p1 = asm_.BuildControlMessagePackets(first);
    std::vector<Packet> p2 = asm_.BuildControlMessagePackets(second);

    ASSERT_FALSE(p1.empty());
    ASSERT_FALSE(p1[0].bunches.empty());
    EXPECT_FALSE(p1[0].bunches[0].bControl);  // server never opens ch0
    EXPECT_FALSE(p1[0].bunches[0].bOpen);
    EXPECT_EQ(p1[0].bunches[0].chSequence, 1u);

    ASSERT_FALSE(p2.empty());
    ASSERT_FALSE(p2[0].bunches.empty());
    const Bunch& b2 = p2[0].bunches[0];
    EXPECT_FALSE(b2.bControl);
    EXPECT_FALSE(b2.bOpen);
    EXPECT_TRUE(b2.bReliable);
    EXPECT_EQ(b2.chSequence, 2u); // continues from the first message
}

// QueueAck values appear in the produced packet's acks and are cleared after.
TEST(PacketAssembler, QueuedAcksDrainOntoPacket) {
    PacketAssembler asm_;
    asm_.QueueAck(5);
    asm_.QueueAck(7);
    EXPECT_EQ(asm_.PendingAckCount(), 2u);

    std::vector<Packet> packets = asm_.BuildControlMessagePackets({0x00, 0x00});

    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets[0].acks, (std::vector<uint32_t>{5u, 7u}));
    EXPECT_EQ(asm_.PendingAckCount(), 0u);

    // A subsequent build has no leftover acks.
    std::vector<Packet> packets2 = asm_.BuildControlMessagePackets({0x01, 0x01});
    ASSERT_FALSE(packets2.empty());
    EXPECT_TRUE(packets2[0].acks.empty());
}

// PacketIds increment across produced packets (within and across calls).
TEST(PacketAssembler, PacketIdsIncrement) {
    PacketAssembler asm_;
    // A long payload to force multiple packets in a single call.
    std::vector<uint8_t> payload(16, 0x5A); // 128 bits => multiple bunches/packets

    std::vector<Packet> packets = asm_.BuildControlMessagePackets(payload);
    ASSERT_GE(packets.size(), 2u);
    for (size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].packetId, static_cast<uint32_t>(i)) << "packet " << i;
    }

    // Next call continues the PacketId sequence.
    uint32_t expectNext = static_cast<uint32_t>(packets.size());
    std::vector<Packet> more = asm_.BuildControlMessagePackets({0x02, 0x02});
    ASSERT_FALSE(more.empty());
    EXPECT_EQ(more[0].packetId, expectNext);
}

// BuildAckOnlyPacket flushes pending acks with no bunch.
TEST(PacketAssembler, AckOnlyPacketHasNoBunch) {
    PacketAssembler asm_;
    asm_.QueueAck(11);
    asm_.QueueAck(13);

    Packet pkt = asm_.BuildAckOnlyPacket();

    EXPECT_TRUE(pkt.bunches.empty());
    EXPECT_EQ(pkt.acks, (std::vector<uint32_t>{11u, 13u}));
    EXPECT_EQ(pkt.packetId, 0u);
    EXPECT_EQ(asm_.PendingAckCount(), 0u);

    // It consumed a PacketId: the next data packet uses id 1.
    std::vector<Packet> packets = asm_.BuildControlMessagePackets({0x00, 0x00});
    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets[0].packetId, 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
