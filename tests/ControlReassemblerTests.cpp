// tests/ControlReassemblerTests.cpp
//
// Tests for the inbound control-channel reassembler. The real RS2 client sends
// each control-channel message as ONE reliable bunch (at the established-phase
// MaxPacket of 2048 a single bunch holds up to 16384 data bits, so even the big
// EOS auth-ticket message is not fragmented). So the reassembler's contract is
// PER-BUNCH delivery: each reliable control-channel (chIndex 0) bunch payload is
// delivered as one complete message, in ChSequence order, with duplicates
// ignored. Because UE3's reliable ChSequence is a connection-global counter
// shared across channels, the control channel's own bunches arrive with GAPS
// (sequence numbers consumed by other channels, or lost) - so the reassembler
// must SKIP a persistent gap rather than deadlock waiting for it.

#include <gtest/gtest.h>

#include "Network/ControlReassembler.h"
#include "Network/PacketCodec.h"
#include "Network/ControlChannel.h"
#include "Network/NetMessages.h"

#include <vector>
#include <cstdint>

using PacketCodec::Bunch;
using PacketCodec::ControlReassembler;

namespace {

// Build one reliable control-channel (chIndex 0, chType 1) bunch carrying
// `payload` at ChSequence `seq`. This is exactly the shape PacketCodec::Decode
// produces for an inbound control message bunch.
Bunch MakeControlBunch(uint32_t seq, const std::vector<uint8_t>& payload) {
    Bunch b;
    b.bControl = false;
    b.bReliable = true;
    b.chIndex = static_cast<uint32_t>(kControlChannelIndex);
    b.chType = PacketCodec::kControlChannelType;
    b.chSequence = seq;
    b.payload = payload;
    b.payloadBits = static_cast<uint32_t>(payload.size() * 8);
    return b;
}

std::vector<uint8_t> SampleHello() {
    ControlChannel::HelloMessage h;
    h.minVersion = 7038;
    h.version = 7258;
    h.steamId = 0x1100001020304050ull;
    h.leechSessionId = "leech-session-abc";
    h.token = "tok";
    return ControlChannel::BuildHello(h);
}

std::vector<uint8_t> SampleNetspeed() {
    ControlChannel::NetspeedMessage ns;
    ns.netspeed = 80000;
    return ControlChannel::BuildNetspeed(ns);
}

} // namespace

// A single reliable control bunch is delivered as one message with its exact
// payload. The reassembler's stream starts at ChSequence 1.
TEST(ControlReassembler, DeliversSingleBunchMessage) {
    const std::vector<uint8_t> hello = SampleHello();

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    re.OnBunch(MakeControlBunch(1, hello));

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], hello);
}

// Bunches arriving out of ChSequence order are buffered and delivered in order.
TEST(ControlReassembler, HandlesOutOfOrderBunches) {
    const std::vector<uint8_t> m1 = SampleHello();
    const std::vector<uint8_t> m2 = SampleNetspeed();
    const std::vector<uint8_t> m3 = {0x06, 0xAA, 0xBB};  // arbitrary 3rd message

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    // Deliver out of order: 3, 1, 2. Nothing is emitted until seq 1 arrives.
    re.OnBunch(MakeControlBunch(3, m3));
    re.OnBunch(MakeControlBunch(1, m1));
    re.OnBunch(MakeControlBunch(2, m2));

    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], m1);
    EXPECT_EQ(got[1], m2);
    EXPECT_EQ(got[2], m3);
}

// A duplicated (retransmitted) bunch is delivered only once.
TEST(ControlReassembler, IgnoresDuplicateRetransmits) {
    const std::vector<uint8_t> m1 = SampleHello();
    const std::vector<uint8_t> m2 = SampleNetspeed();

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    re.OnBunch(MakeControlBunch(1, m1));
    re.OnBunch(MakeControlBunch(1, m1));  // duplicate retransmit of seq 1
    re.OnBunch(MakeControlBunch(2, m2));
    re.OnBunch(MakeControlBunch(2, m2));  // duplicate retransmit of seq 2

    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], m1);
    EXPECT_EQ(got[1], m2);
}

// Two messages on one continuous reliable stream are both delivered, in order.
TEST(ControlReassembler, DeliversTwoBackToBackMessages) {
    const std::vector<uint8_t> hello = SampleHello();
    const std::vector<uint8_t> netspeed = SampleNetspeed();

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    re.OnBunch(MakeControlBunch(1, hello));
    re.OnBunch(MakeControlBunch(2, netspeed));

    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], hello);
    EXPECT_EQ(got[1], netspeed);
}

// Non-control (chIndex != 0) and unreliable bunches are ignored.
TEST(ControlReassembler, IgnoresNonControlAndUnreliableBunches) {
    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });

    Bunch otherChannel = MakeControlBunch(1, {0x01, 0x02});
    otherChannel.chIndex = 5;  // an actor channel, not the control channel
    re.OnBunch(otherChannel);

    Bunch unreliable = MakeControlBunch(1, {0x03, 0x04});
    unreliable.bReliable = false;
    re.OnBunch(unreliable);

    EXPECT_TRUE(got.empty());
}

// A persistent gap in the (connection-global) ChSequence - e.g. seq 2 went to
// another channel or was lost - must be skipped once enough later control
// bunches accumulate, so it never deadlocks the control stream. (Observed live:
// login completed at seq 3, then seqs 5,6,7,... piled up unread because seq 4
// never arrived on the control channel.)
TEST(ControlReassembler, SkipsPersistentSequenceGap) {
    const std::vector<uint8_t> m1 = {0x00, 0x11};
    const std::vector<uint8_t> m3 = {0x03, 0x33};
    const std::vector<uint8_t> m4 = {0x04, 0x44};
    const std::vector<uint8_t> m5 = {0x05, 0x55};
    const std::vector<uint8_t> m6 = {0x06, 0x66};

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });

    re.OnBunch(MakeControlBunch(1, m1));  // delivered immediately; m_nextSeq -> 2
    ASSERT_EQ(got.size(), 1u);

    // seq 2 never arrives (gap). Buffer 3,4,5,6. Once the skip threshold (4) is
    // reached, the reassembler jumps past the gap to the lowest buffered seq and
    // drains the rest.
    re.OnBunch(MakeControlBunch(3, m3));
    re.OnBunch(MakeControlBunch(4, m4));
    re.OnBunch(MakeControlBunch(5, m5));
    EXPECT_EQ(got.size(), 1u) << "must still be waiting for the gap before threshold";
    re.OnBunch(MakeControlBunch(6, m6));  // 4 buffered -> skip the gap

    ASSERT_EQ(got.size(), 5u);
    EXPECT_EQ(got[0], m1);
    EXPECT_EQ(got[1], m3);
    EXPECT_EQ(got[2], m4);
    EXPECT_EQ(got[3], m5);
    EXPECT_EQ(got[4], m6);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
