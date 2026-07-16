// tests/PacketAssemblerTests.cpp
//
// Tests for the OUTBOUND control-channel packet assembler
// (src/Network/PacketAssembler). These exercise the STRUCT-level contract
// (PacketCodec::Packet / Bunch) directly — they do NOT depend on
// PacketCodec::Encode except where a wire round-trip is explicitly required.
//
// Wire rules under test (docs/RS2V_ControlChannel_WireSpec_7258.md §2/§3/§5):
//   * one logical control payload must fit one complete reliable bunch;
//   * reliable control bunches use ChIndex 0, ChType 1, modulo-1024 ChSequence
//     starting at 1 and wrapping 1023 -> 0;
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
using PacketCodec::ControlMessageBuildError;
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

} // namespace

// A short payload (2 bytes) -> exactly 1 packet, 1 bunch on the (already-open,
// client-opened) control channel: bControl=0, bOpen=0.
TEST(PacketAssembler, ShortPayloadSingleBunch) {
    PacketAssembler asm_;
    const std::vector<uint8_t> payload = {NMTByte(NMT::Hello), 0x01}; // 16 bits

    const auto built = asm_.BuildControlMessagePackets(payload);
    ASSERT_TRUE(built.has_value());
    const std::vector<Packet>& packets = *built;

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

// UE3 does not concatenate arbitrary control-message fragments across bunches.
// Reject an oversized logical payload atomically so the caller can split at a
// real message/record boundary.
TEST(PacketAssembler, RejectsOversizedMessageWithoutConsumingStateOrAcks) {
    PacketAssembler asm_;
    asm_.QueueAck(7u);
    const std::vector<uint8_t> payload(8u, 0xA5u); // 64 bits reaches legacy MaxPacket=8 bound

    const auto rejected =
        asm_.BuildControlMessagePackets(payload, 8u);

    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ControlMessageBuildError::PayloadTooLarge);
    EXPECT_EQ(asm_.NextPacketId(), 0u);
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_FALSE(asm_.IsChannelOpened());
    EXPECT_EQ(asm_.PendingAckCount(), 1u);

    // A later valid message receives the untouched first identifiers and ack.
    const auto accepted =
        asm_.BuildControlMessagePackets({0xA5u}, 16u);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->size(), 1u);
    EXPECT_EQ((*accepted)[0].packetId, 0u);
    EXPECT_EQ((*accepted)[0].acks, (std::vector<uint32_t>{7u}));
    ASSERT_EQ((*accepted)[0].bunches.size(), 1u);
    EXPECT_EQ((*accepted)[0].bunches[0].chSequence, 1u);
    EXPECT_EQ((*accepted)[0].bunches[0].payload,
              (std::vector<uint8_t>{0xA5u}));
}

TEST(PacketAssembler, PreparedMessageCancelsWithoutPublishingSequenceOrAcks) {
    PacketAssembler asm_;
    asm_.QueueAck(9u);

    auto prepared =
        asm_.PrepareControlMessagePackets({0xA5u});
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->IsActive());
    ASSERT_EQ(prepared->Packets().size(), 1u);
    EXPECT_EQ(prepared->Packets().front().outboundPacketSerial, 0);
    EXPECT_LE(prepared->WireBytes().size(),
              PacketCodec::kServerSendMaxPacketBytes);
    EXPECT_EQ(asm_.NextPacketId(), 0u);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);

    ASSERT_TRUE(
        asm_.CancelPreparedControlMessage(*prepared).has_value());
    EXPECT_FALSE(prepared->IsActive());
    EXPECT_EQ(asm_.NextPacketId(), 0u);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);
}

TEST(PacketAssembler,
     PreparedControlMessageWithLeadingBunchesPreservesOrderAndPacketIdentity) {
    PacketAssembler asm_;

    Bunch channelOpen;
    channelOpen.bControl = true;
    channelOpen.bOpen = true;
    channelOpen.bReliable = true;
    channelOpen.chIndex = 2u;
    channelOpen.chType = 2u;
    channelOpen.chSequence = 41u;
    channelOpen.payload = {0x15u};
    channelOpen.payloadBits = 5u;

    const std::vector<uint8_t> controlPayload = {0x24u, 0xA5u};
    auto prepared = asm_.PrepareControlMessagePacketWithLeadingBunches(
        {channelOpen}, controlPayload);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(prepared->Packets().size(), 1u);

    const Packet& packet = prepared->Packets().front();
    EXPECT_EQ(packet.packetId, 0u);
    EXPECT_EQ(packet.outboundPacketSerial, 0);
    ASSERT_EQ(packet.bunches.size(), 2u);

    const Bunch& leading = packet.bunches[0];
    EXPECT_TRUE(leading.bControl);
    EXPECT_TRUE(leading.bOpen);
    EXPECT_TRUE(leading.bReliable);
    EXPECT_EQ(leading.chIndex, 2u);
    EXPECT_EQ(leading.chType, 2u);
    EXPECT_EQ(leading.chSequence, 41u);
    EXPECT_EQ(leading.payload, (std::vector<uint8_t>{0x15u}));
    EXPECT_EQ(leading.payloadBits, 5u);

    const Bunch& control = packet.bunches[1];
    EXPECT_FALSE(control.bControl);
    EXPECT_FALSE(control.bOpen);
    EXPECT_TRUE(control.bReliable);
    EXPECT_EQ(control.chIndex, kControlChannelIndex);
    EXPECT_EQ(control.chType, PacketCodec::kControlChannelType);
    EXPECT_EQ(control.chSequence, 1u);
    EXPECT_EQ(control.payload, controlPayload);

    const Packet decoded = PacketCodec::Decode(
        prepared->WireBytes().data(), prepared->WireBytes().size(),
        PacketCodec::kServerSendMaxPacketBytes);
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded.packetId, packet.packetId);
    ASSERT_EQ(decoded.bunches.size(), 2u);
    EXPECT_EQ(decoded.bunches[0].chIndex, 2u);
    EXPECT_TRUE(decoded.bunches[0].bOpen);
    EXPECT_EQ(decoded.bunches[1].chIndex, kControlChannelIndex);
    EXPECT_EQ(decoded.bunches[1].payload, controlPayload);

    // Preparation reserves (but does not publish) the ch0 sequence. PacketId
    // and ACK state remain untouched, and cancellation restores the cursor.
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.NextControlChSequence(), 2u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 1u);
    ASSERT_TRUE(
        asm_.CancelPreparedControlMessage(*prepared).has_value());
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);
}

TEST(PacketAssembler,
     CommittingControlMessageWithLeadingBunchesOwnsControlSequence) {
    PacketAssembler asm_;

    Bunch channelOpen;
    channelOpen.bControl = true;
    channelOpen.bOpen = true;
    channelOpen.bReliable = true;
    channelOpen.chIndex = 2u;
    channelOpen.chType = 2u;
    channelOpen.chSequence = 7u;

    auto prepared = asm_.PrepareControlMessagePacketWithLeadingBunches(
        {channelOpen}, {0x24u});
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 1u);

    ASSERT_TRUE(
        asm_.CommitPreparedControlMessage(*prepared).has_value());
    EXPECT_FALSE(prepared->IsActive());
    EXPECT_EQ(asm_.NextPacketSerial(), 1);
    EXPECT_EQ(asm_.NextControlChSequence(), 2u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 1u);
    EXPECT_TRUE(asm_.IsChannelOpened());
}

TEST(PacketAssembler,
     LeadingBunchPreparationFitsAckPrefixAndRejectsOversizeAtomically) {
    PacketAssembler asm_;
    for (uint32_t packetId = 0u; packetId < 900u; ++packetId) {
        asm_.QueueAck(packetId);
    }

    Bunch channelOpen;
    channelOpen.bControl = true;
    channelOpen.bOpen = true;
    channelOpen.bReliable = true;
    channelOpen.chIndex = 2u;
    channelOpen.chType = 2u;
    channelOpen.chSequence = 1u;
    channelOpen.payload.assign(1300u, 0x3Cu);
    channelOpen.payloadBits =
        static_cast<uint32_t>(channelOpen.payload.size() * 8u);

    auto prepared = asm_.PrepareControlMessagePacketWithLeadingBunches(
        {channelOpen}, {0x24u});
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(prepared->Packets().size(), 1u);
    const std::vector<uint32_t>& fittedAcks =
        prepared->Packets().front().acks;
    EXPECT_FALSE(fittedAcks.empty());
    EXPECT_LT(fittedAcks.size(), 900u);
    EXPECT_EQ(fittedAcks.front(), 0u);
    EXPECT_EQ(fittedAcks.back(),
              static_cast<uint32_t>(fittedAcks.size() - 1u));
    EXPECT_LE(prepared->WireBytes().size(),
              PacketCodec::kServerSendMaxPacketBytes);

    // Neither the fitted prefix nor the reserved ch0 sequence is published
    // until commit. The unpublished reservation is visible to the allocator,
    // and cancellation restores the full pre-prepare state.
    EXPECT_EQ(asm_.PendingAckCount(), 900u);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.NextControlChSequence(), 2u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 1u);
    ASSERT_TRUE(
        asm_.CancelPreparedControlMessage(*prepared).has_value());
    EXPECT_EQ(asm_.PendingAckCount(), 900u);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);

    Bunch oversized = channelOpen;
    oversized.payload.assign(1499u, 0xA5u);
    oversized.payloadBits =
        static_cast<uint32_t>(oversized.payload.size() * 8u);
    const auto rejected =
        asm_.PrepareControlMessagePacketWithLeadingBunches(
            {oversized}, {0x24u});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ControlMessageBuildError::PayloadTooLarge);
    EXPECT_EQ(asm_.PendingAckCount(), 900u);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);
}

TEST(PacketAssembler, RejectsPayloadThatFitsDataBoundButNotFramedPacket) {
    PacketAssembler asm_;
    asm_.QueueAck(11u);

    // 1499 bytes is below the raw BunchDataBits value bound (11999 bits), but
    // PacketId + ACK + bunch header + terminator push the datagram past 1500.
    const auto rejected = asm_.BuildControlMessagePackets(
        std::vector<uint8_t>(1499u, 0x5Au));

    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ControlMessageBuildError::PayloadTooLarge);
    EXPECT_EQ(asm_.NextPacketId(), 0u);
    EXPECT_EQ(asm_.NextControlChSequence(), 1u);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);
}

TEST(PacketAssembler, OversizeControlPreflightWinsOverReliableBackpressure) {
    PacketAssembler asm_;
    for (uint32_t sequence = 1u;
         sequence < PacketCodec::kReliableBuffer;
         ++sequence) {
        ASSERT_TRUE(
            asm_.BuildControlMessagePackets({0x5Au}).has_value());
    }
    ASSERT_EQ(asm_.AvailableControlCapacity(), 0u);
    asm_.QueueAck(31u);

    const std::vector<uint8_t> oversized(1499u, 0xA5u);
    const auto preflight =
        asm_.ValidateControlMessagePacket(oversized);
    ASSERT_FALSE(preflight.has_value());
    EXPECT_EQ(preflight.error(),
              ControlMessageBuildError::PayloadTooLarge);

    const auto prepared =
        asm_.PrepareControlMessagePackets(oversized);
    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error(),
              ControlMessageBuildError::PayloadTooLarge);
    EXPECT_EQ(asm_.NextPacketSerial(), 127);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 127u);
}

TEST(PacketAssembler, PiggybacksOnlyAckPrefixThatFitsMaxPacket) {
    PacketAssembler asm_;
    for (uint32_t packetId = 0u; packetId < 900u; ++packetId) {
        asm_.QueueAck(packetId);
    }

    const auto built = asm_.BuildControlMessagePackets(
        std::vector<uint8_t>(1400u, 0x3Cu));

    ASSERT_TRUE(built.has_value());
    ASSERT_EQ(built->size(), 1u);
    EXPECT_FALSE((*built)[0].acks.empty());
    EXPECT_LT((*built)[0].acks.size(), 900u);
    EXPECT_EQ(asm_.PendingAckCount(),
              900u - (*built)[0].acks.size());
    const std::vector<uint8_t> wire = PacketCodec::Encode(
        (*built)[0], PacketCodec::kServerSendMaxPacketBytes);
    EXPECT_LE(wire.size(), PacketCodec::kServerSendMaxPacketBytes);
}

TEST(PacketAssembler, RawBunchPiggybacksOnlyFittingAckPrefix) {
    PacketAssembler asm_;
    for (uint32_t packetId = 0u; packetId < 900u; ++packetId) {
        asm_.QueueAck(packetId);
    }

    Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = 2u;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload.assign(1400u, 0x3Cu);
    bunch.payloadBits = static_cast<uint32_t>(bunch.payload.size() * 8u);

    const auto built = asm_.BuildRawBunchPacket(bunch);
    ASSERT_TRUE(built.has_value());
    EXPECT_FALSE(built->acks.empty());
    EXPECT_LT(built->acks.size(), 900u);
    EXPECT_EQ(built->acks.front(), 0u);
    EXPECT_EQ(built->acks.back(),
              static_cast<uint32_t>(built->acks.size() - 1u));
    EXPECT_EQ(asm_.PendingAckCount(), 900u - built->acks.size());
    const std::vector<uint8_t> wire = PacketCodec::Encode(
        *built, PacketCodec::kServerSendMaxPacketBytes);
    EXPECT_LE(wire.size(), PacketCodec::kServerSendMaxPacketBytes);
}

TEST(PacketAssembler, RawBunchesPiggybackOnlyFittingAckPrefix) {
    PacketAssembler asm_;
    for (uint32_t packetId = 0u; packetId < 900u; ++packetId) {
        asm_.QueueAck(packetId);
    }

    Bunch bunch;
    bunch.chIndex = 2u;
    bunch.payload.assign(700u, 0xC3u);
    bunch.payloadBits = static_cast<uint32_t>(bunch.payload.size() * 8u);

    const auto built = asm_.BuildRawBunchesPacket({bunch, bunch});
    ASSERT_TRUE(built.has_value());
    EXPECT_FALSE(built->acks.empty());
    EXPECT_LT(built->acks.size(), 900u);
    EXPECT_EQ(built->acks.front(), 0u);
    EXPECT_EQ(built->acks.back(),
              static_cast<uint32_t>(built->acks.size() - 1u));
    EXPECT_EQ(asm_.PendingAckCount(), 900u - built->acks.size());
    const std::vector<uint8_t> wire = PacketCodec::Encode(
        *built, PacketCodec::kServerSendMaxPacketBytes);
    EXPECT_LE(wire.size(), PacketCodec::kServerSendMaxPacketBytes);
}

TEST(PacketAssembler, OversizedRawPacketsAreAtomic) {
    Bunch oversized;
    oversized.bReliable = true;
    oversized.chIndex = 2u;
    oversized.chType = 2u;
    oversized.chSequence = 1u;
    oversized.payload.assign(1499u, 0xA5u);
    oversized.payloadBits =
        static_cast<uint32_t>(oversized.payload.size() * 8u);

    PacketAssembler single;
    single.QueueAck(7u);
    const auto rejectedSingle =
        single.BuildRawBunchPacket(oversized);
    ASSERT_FALSE(rejectedSingle.has_value());
    EXPECT_EQ(rejectedSingle.error(),
              PacketCodec::OutboundPacketBuildError::PacketTooLarge);
    EXPECT_EQ(single.NextPacketSerial(), 0);
    EXPECT_EQ(single.PendingAckCount(), 1u);

    PacketAssembler multiple;
    multiple.QueueAck(9u);
    Bunch half = oversized;
    half.payload.resize(800u);
    half.payloadBits =
        static_cast<uint32_t>(half.payload.size() * 8u);
    const auto rejectedMultiple =
        multiple.BuildRawBunchesPacket({half, half});
    ASSERT_FALSE(rejectedMultiple.has_value());
    EXPECT_EQ(rejectedMultiple.error(),
              PacketCodec::OutboundPacketBuildError::PacketTooLarge);
    EXPECT_EQ(multiple.NextPacketSerial(), 0);
    EXPECT_EQ(multiple.PendingAckCount(), 1u);
}

// Consecutive BuildControlMessagePackets calls: neither opens the channel and
// ChSequence continues incrementing across messages.
TEST(PacketAssembler, ChSequenceContinuesAcrossMessages) {
    PacketAssembler asm_;
    const std::vector<uint8_t> first = {NMTByte(NMT::Challenge), 0x02};
    const std::vector<uint8_t> second = {NMTByte(NMT::Welcome), 0x03};

    const auto built1 = asm_.BuildControlMessagePackets(first);
    const auto built2 = asm_.BuildControlMessagePackets(second);
    ASSERT_TRUE(built1.has_value());
    ASSERT_TRUE(built2.has_value());
    const std::vector<Packet>& p1 = *built1;
    const std::vector<Packet>& p2 = *built2;

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

    const auto built =
        asm_.BuildControlMessagePackets({0x00, 0x00});

    ASSERT_TRUE(built.has_value());
    const std::vector<Packet>& packets = *built;
    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets[0].acks, (std::vector<uint32_t>{5u, 7u}));
    EXPECT_EQ(asm_.PendingAckCount(), 0u);

    // A subsequent build has no leftover acks.
    const auto built2 =
        asm_.BuildControlMessagePackets({0x01, 0x01});
    ASSERT_TRUE(built2.has_value());
    const std::vector<Packet>& packets2 = *built2;
    ASSERT_FALSE(packets2.empty());
    EXPECT_TRUE(packets2[0].acks.empty());
}

// PacketIds increment across produced packets.
TEST(PacketAssembler, PacketIdsIncrement) {
    PacketAssembler asm_;
    const auto built1 =
        asm_.BuildControlMessagePackets({0x01u});
    const auto built2 =
        asm_.BuildControlMessagePackets({0x02u});
    ASSERT_TRUE(built1.has_value());
    ASSERT_TRUE(built2.has_value());
    const std::vector<Packet>& first = *built1;
    const std::vector<Packet>& second = *built2;
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].packetId, 0u);
    EXPECT_EQ(second[0].packetId, 1u);
    EXPECT_EQ(asm_.NextPacketId(), 2u);
}

TEST(PacketAssembler, ReliableWindowRetainsOutOfOrderAckTombstones) {
    PacketAssembler asm_;

    for (uint32_t sequence = 1u;
         sequence <= PacketCodec::kReliableBuffer - 1u;
         ++sequence) {
        const auto built = asm_.BuildControlMessagePackets({0x5Au});
        ASSERT_TRUE(built.has_value());
        ASSERT_EQ(built->size(), 1u);
        ASSERT_EQ((*built)[0].bunches.size(), 1u);
        EXPECT_EQ((*built)[0].bunches[0].chSequence, sequence);
    }
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 127u);
    EXPECT_EQ(asm_.ControlIssuanceWindowSize(), 127u);
    EXPECT_EQ(asm_.AvailableControlCapacity(), 0u);
    EXPECT_EQ(asm_.NextControlChSequence(), 128u);
    EXPECT_EQ(asm_.NextPacketId(), 127u);

    asm_.QueueAck(77u);
    const auto full = asm_.BuildControlMessagePackets({0xA0u});
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), ControlMessageBuildError::ReliableWindowFull);
    EXPECT_EQ(asm_.NextControlChSequence(), 128u);
    EXPECT_EQ(asm_.NextPacketId(), 127u);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);

    // ACK every successor while retaining the oldest gap. They stop counting as
    // in-flight but remain window tombstones, so capacity must stay closed.
    for (uint32_t sequence = 2u;
         sequence <= PacketCodec::kReliableBuffer - 1u;
         ++sequence) {
        ASSERT_TRUE(
            asm_.AcknowledgeControlSequence(sequence).has_value());
    }
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 1u);
    EXPECT_EQ(asm_.ControlIssuanceWindowSize(), 127u);
    EXPECT_EQ(asm_.AvailableControlCapacity(), 0u);
    const auto stillFull = asm_.BuildControlMessagePackets({0xA1u});
    ASSERT_FALSE(stillFull.has_value());
    EXPECT_EQ(stillFull.error(),
              ControlMessageBuildError::ReliableWindowFull);

    ASSERT_TRUE(asm_.AcknowledgeControlSequence(1u).has_value());
    EXPECT_EQ(asm_.OutstandingControlBunchCount(), 0u);
    EXPECT_EQ(asm_.ControlIssuanceWindowSize(), 0u);
    EXPECT_EQ(asm_.AvailableControlCapacity(), 127u);

    const auto resumed = asm_.BuildControlMessagePackets({0xA2u});
    ASSERT_TRUE(resumed.has_value());
    ASSERT_EQ(resumed->size(), 1u);
    EXPECT_EQ((*resumed)[0].packetId, 127u);
    EXPECT_EQ((*resumed)[0].acks, (std::vector<uint32_t>{77u}));
    ASSERT_EQ((*resumed)[0].bunches.size(), 1u);
    EXPECT_EQ((*resumed)[0].bunches[0].chSequence, 128u);
    ASSERT_TRUE(asm_.AcknowledgeControlSequence(128u).has_value());
}

TEST(PacketAssembler, ControlChSequenceWrapsModulo1024OnWire) {
    PacketAssembler asm_;

    // Consume sequences 1..1022, leaving the cursor at the wrap boundary.
    for (uint32_t expected = 1u;
         expected < PacketCodec::kMaxChSequence - 1u;
         ++expected) {
        const auto built =
            asm_.BuildControlMessagePackets({0x5Au});
        ASSERT_TRUE(built.has_value());
        const std::vector<Packet>& packets = *built;
        ASSERT_EQ(packets.size(), 1u);
        ASSERT_EQ(packets[0].bunches.size(), 1u);
        EXPECT_EQ(packets[0].bunches[0].chSequence, expected);
        ASSERT_TRUE(
            asm_.AcknowledgeControlSequence(expected).has_value());
    }
    EXPECT_EQ(asm_.NextControlChSequence(), 1023u);

    // Non-control packet builders own PacketId only and must not consume ch0.
    asm_.QueueAck(77u);
    const auto builtAckOnly = asm_.BuildAckOnlyPacket();
    ASSERT_TRUE(builtAckOnly.has_value());
    const Packet& ackOnly = *builtAckOnly;
    EXPECT_EQ(ackOnly.acks, (std::vector<uint32_t>{77u}));
    EXPECT_EQ(asm_.NextControlChSequence(), 1023u);

    Bunch actor;
    actor.bReliable = true;
    actor.chIndex = 2u;
    actor.chSequence = 55u;
    ASSERT_TRUE(asm_.BuildRawBunchPacket(actor).has_value());
    ASSERT_TRUE(asm_.BuildRawBunchesPacket({actor}).has_value());
    EXPECT_EQ(asm_.NextControlChSequence(), 1023u);

    const std::vector<std::vector<uint8_t>> payloads = {
        {}, {0xA0u}, {0xA1u},
    };
    const uint32_t expectedSequences[] = {1023u, 0u, 1u};
    const uint32_t expectedCursors[] = {0u, 1u, 2u};
    for (size_t index = 0; index < payloads.size(); ++index) {
        const auto built =
            asm_.BuildControlMessagePackets(payloads[index]);
        ASSERT_TRUE(built.has_value());
        const std::vector<Packet>& packets = *built;
        ASSERT_EQ(packets.size(), 1u);
        ASSERT_EQ(packets[0].bunches.size(), 1u);
        EXPECT_EQ(packets[0].bunches[0].chSequence,
                  expectedSequences[index]);
        EXPECT_EQ(asm_.NextControlChSequence(), expectedCursors[index]);

        const std::vector<uint8_t> wire = PacketCodec::Encode(
            packets[0], PacketCodec::kServerSendMaxPacketBytes);
        const Packet decoded = PacketCodec::Decode(
            wire.data(), wire.size(), PacketCodec::kServerSendMaxPacketBytes);
        ASSERT_TRUE(decoded.ok);
        ASSERT_EQ(decoded.bunches.size(), 1u);
        EXPECT_EQ(decoded.bunches[0].chSequence,
                  expectedSequences[index]);
        ASSERT_TRUE(asm_.AcknowledgeControlSequence(
            expectedSequences[index]).has_value());
    }
}

// BuildAckOnlyPacket flushes pending acks with no bunch.
TEST(PacketAssembler, AckOnlyPacketHasNoBunch) {
    PacketAssembler asm_;
    asm_.QueueAck(11);
    asm_.QueueAck(13);

    const auto builtAckOnly = asm_.BuildAckOnlyPacket();
    ASSERT_TRUE(builtAckOnly.has_value());
    const Packet& pkt = *builtAckOnly;

    EXPECT_TRUE(pkt.bunches.empty());
    EXPECT_EQ(pkt.acks, (std::vector<uint32_t>{11u, 13u}));
    EXPECT_EQ(pkt.packetId, 0u);
    EXPECT_EQ(asm_.PendingAckCount(), 0u);

    // It consumed a PacketId: the next data packet uses id 1.
    const auto built =
        asm_.BuildControlMessagePackets({0x00, 0x00});
    ASSERT_TRUE(built.has_value());
    const std::vector<Packet>& packets = *built;
    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets[0].packetId, 1u);
}

TEST(PacketAssembler, AckOnlyPacketRetainsNonFittingAckSuffix) {
    PacketAssembler asm_;
    for (uint32_t packetId = 0u; packetId < 100u; ++packetId) {
        asm_.QueueAck(packetId);
    }

    const auto built = asm_.BuildAckOnlyPacket(32u);
    ASSERT_TRUE(built.has_value());
    EXPECT_FALSE(built->acks.empty());
    EXPECT_LT(built->acks.size(), 100u);
    EXPECT_EQ(built->acks.front(), 0u);
    EXPECT_EQ(built->acks.back(),
              static_cast<uint32_t>(built->acks.size() - 1u));
    EXPECT_EQ(asm_.PendingAckCount(), 100u - built->acks.size());
    const std::vector<uint8_t> wire = PacketCodec::Encode(*built, 32u);
    EXPECT_LE(wire.size(), 32u);

    const size_t firstPrefixSize = built->acks.size();
    const auto next = asm_.BuildAckOnlyPacket(32u);
    ASSERT_TRUE(next.has_value());
    ASSERT_FALSE(next->acks.empty());
    EXPECT_EQ(next->acks.front(),
              static_cast<uint32_t>(firstPrefixSize));
}

TEST(PacketAssembler, OversizedAckOnlyPacketIsAtomic) {
    PacketAssembler asm_;
    asm_.QueueAck(17u);

    // PacketId plus terminator already requires two bytes, even with no ACKs.
    const auto rejected = asm_.BuildAckOnlyPacket(1u);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(),
              PacketCodec::OutboundPacketBuildError::PacketTooLarge);
    EXPECT_EQ(asm_.NextPacketSerial(), 0);
    EXPECT_EQ(asm_.PendingAckCount(), 1u);

    const auto accepted = asm_.BuildAckOnlyPacket(8u);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->outboundPacketSerial, 0);
    EXPECT_EQ(accepted->acks, (std::vector<uint32_t>{17u}));
    EXPECT_EQ(asm_.PendingAckCount(), 0u);
}

TEST(PacketAssembler, PacketSerialWrapsOnlyOnWireAndAckUnwrapsGeneration) {
    PacketAssembler asm_;

    for (int64_t serial = 0;
         serial <= static_cast<int64_t>(kMaxPacketId) + 2;
         ++serial) {
        const auto built = asm_.BuildAckOnlyPacket();
        ASSERT_TRUE(built.has_value());
        EXPECT_EQ(built->outboundPacketSerial, serial);
        EXPECT_EQ(
            built->packetId,
            static_cast<uint32_t>(
                serial % static_cast<int64_t>(kMaxPacketId)));

        const auto resolved = asm_.ResolveOutboundAck(built->packetId);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, serial);
    }

    EXPECT_EQ(asm_.HighestResolvedAckSerial(),
              static_cast<int64_t>(kMaxPacketId) + 2);
    // A reordered pre-wrap ACK resolves to its older generation and cannot move
    // the high-water mark backwards.
    const auto late = asm_.ResolveOutboundAck(
        static_cast<uint32_t>(kMaxPacketId - 1u));
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(*late,
              static_cast<int64_t>(kMaxPacketId) - 1);
    EXPECT_EQ(asm_.HighestResolvedAckSerial(),
              static_cast<int64_t>(kMaxPacketId) + 2);
}

TEST(PacketAssembler, InvalidFutureAndHalfRangeAcksDoNotPoisonReference) {
    PacketAssembler asm_;
    const auto first = asm_.BuildAckOnlyPacket();
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->outboundPacketSerial, 0);

    const auto future = asm_.ResolveOutboundAck(1u);
    ASSERT_FALSE(future.has_value());
    EXPECT_EQ(future.error(), PacketCodec::OutboundAckError::FuturePacket);
    EXPECT_EQ(asm_.HighestResolvedAckSerial(), -1);

    const auto half = asm_.ResolveOutboundAck(
        static_cast<uint32_t>(kMaxPacketId / 2u));
    ASSERT_FALSE(half.has_value());
    EXPECT_EQ(half.error(),
              PacketCodec::OutboundAckError::AmbiguousHalfRange);
    EXPECT_EQ(asm_.HighestResolvedAckSerial(), -1);

    const auto before = asm_.ResolveOutboundAck(
        static_cast<uint32_t>(kMaxPacketId - 1u));
    ASSERT_FALSE(before.has_value());
    EXPECT_EQ(before.error(), PacketCodec::OutboundAckError::BeforeSession);
    EXPECT_EQ(asm_.HighestResolvedAckSerial(), -1);

    const auto valid = asm_.ResolveOutboundAck(0u);
    ASSERT_TRUE(valid.has_value());
    EXPECT_EQ(*valid, 0);
    EXPECT_EQ(asm_.HighestResolvedAckSerial(), 0);
}

TEST(PacketAssembler, PacketAllocationStopsBeforeAmbiguousHalfRange) {
    PacketAssembler asm_;
    constexpr int64_t kMaximumUnackedPackets =
        static_cast<int64_t>(kMaxPacketId) / 2 - 1;

    Bunch bunch;
    bunch.chIndex = 2u;

    for (int64_t serial = 0; serial < kMaximumUnackedPackets; ++serial) {
        const auto built = asm_.BuildRawBunchPacket(bunch);
        ASSERT_TRUE(built.has_value());
        EXPECT_EQ(built->outboundPacketSerial, serial);
    }
    EXPECT_FALSE(asm_.HasPacketIdCapacity());
    const auto blocked = asm_.BuildRawBunchPacket(bunch);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(),
              PacketCodec::OutboundPacketBuildError::PacketIdWindowFull);
    EXPECT_EQ(asm_.NextPacketSerial(), kMaximumUnackedPackets);
}

TEST(PacketAssembler, AckOnlyPacketsRetireLocallyAndFreshRetryAckResolves) {
    PacketAssembler asm_;

    Bunch reliable;
    reliable.bReliable = true;
    reliable.chIndex = 2u;
    reliable.chType = 2u;
    reliable.chSequence = 1u;
    reliable.payload = {0xA5u};
    reliable.payloadBits = 8u;

    const auto original = asm_.BuildRawBunchPacket(reliable);
    ASSERT_TRUE(original.has_value());
    ASSERT_EQ(original->outboundPacketSerial, 0);

    constexpr int64_t kBunchlessBuilds =
        static_cast<int64_t>(kMaxPacketId) / 2 + 2;
    for (int64_t index = 0; index < kBunchlessBuilds; ++index) {
        const auto ackOnly = asm_.BuildAckOnlyPacket();
        ASSERT_TRUE(ackOnly.has_value());
        EXPECT_TRUE(ackOnly->bunches.empty());
    }
    EXPECT_TRUE(asm_.HasPacketIdCapacity());
    EXPECT_EQ(asm_.HighestPeerAckSerial(), -1);
    EXPECT_EQ(asm_.AckUnwrapReferenceSerial(), kBunchlessBuilds);

    // A retransmission uses a fresh identity immediately after the local
    // retirement cursor. Its wire ACK therefore unwraps without ambiguity even
    // though the original attempt is now more than half a cycle old.
    const auto retry = asm_.BuildRawBunchPacket(reliable);
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->outboundPacketSerial, kBunchlessBuilds + 1);
    const auto resolved = asm_.ResolveOutboundAck(retry->packetId);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, retry->outboundPacketSerial);
    EXPECT_EQ(asm_.HighestPeerAckSerial(), retry->outboundPacketSerial);
}

RS2V_TEST_MAIN()
