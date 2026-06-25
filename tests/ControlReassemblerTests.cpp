// tests/ControlReassemblerTests.cpp
//
// Tests for the inbound control-channel reassembler. The strongest test is a
// round-trip against the real outbound assembler: a control message fragmented
// by PacketAssembler into bunches must reassemble back to the original message
// payload bit-for-bit. Also covers out-of-order delivery, duplicate retransmits,
// and back-to-back messages on one continuous stream.

#include <gtest/gtest.h>

#include "Network/ControlReassembler.h"
#include "Network/PacketAssembler.h"
#include "Network/PacketCodec.h"
#include "Network/ControlChannel.h"

#include <vector>
#include <cstdint>

using PacketCodec::Bunch;
using PacketCodec::Packet;
using PacketCodec::PacketAssembler;
using PacketCodec::ControlReassembler;

namespace {

// Fragment a message payload into control bunches using the real outbound
// assembler (one bunch per packet), returning the bunches in send order.
std::vector<Bunch> FragmentMessage(PacketAssembler& asm_,
                                   const std::vector<uint8_t>& payload) {
    std::vector<Bunch> bunches;
    for (const Packet& pkt : asm_.BuildControlMessagePackets(payload)) {
        for (const Bunch& b : pkt.bunches) {
            bunches.push_back(b);
        }
    }
    return bunches;
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

} // namespace

// A multi-bunch message reassembles to the exact original payload.
TEST(ControlReassembler, ReassemblesFragmentedHello) {
    const std::vector<uint8_t> hello = SampleHello();
    ASSERT_GT(hello.size(), 7u);  // big enough to require multiple <=63-bit bunches

    PacketAssembler asm_;
    std::vector<Bunch> bunches = FragmentMessage(asm_, hello);
    ASSERT_GT(bunches.size(), 1u);  // it really fragmented

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    for (const Bunch& b : bunches) {
        re.OnBunch(b);
    }

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], hello);
}

// Bunches arriving out of ChSequence order still reassemble correctly.
TEST(ControlReassembler, HandlesOutOfOrderBunches) {
    const std::vector<uint8_t> hello = SampleHello();
    PacketAssembler asm_;
    std::vector<Bunch> bunches = FragmentMessage(asm_, hello);
    ASSERT_GT(bunches.size(), 2u);

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    // Feed in reverse order.
    for (auto it = bunches.rbegin(); it != bunches.rend(); ++it) {
        re.OnBunch(*it);
    }

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], hello);
}

// A duplicated (retransmitted) bunch is processed only once.
TEST(ControlReassembler, IgnoresDuplicateRetransmits) {
    const std::vector<uint8_t> hello = SampleHello();
    PacketAssembler asm_;
    std::vector<Bunch> bunches = FragmentMessage(asm_, hello);

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    // Feed each bunch twice (simulating the client retransmitting un-acked bunches).
    for (const Bunch& b : bunches) {
        re.OnBunch(b);
        re.OnBunch(b);
    }

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], hello);
}

// Two messages on one continuous reliable stream are both delivered, in order.
TEST(ControlReassembler, DeliversTwoBackToBackMessages) {
    const std::vector<uint8_t> hello = SampleHello();
    ControlChannel::NetspeedMessage ns;
    ns.netspeed = 80000;
    const std::vector<uint8_t> netspeed = ControlChannel::BuildNetspeed(ns);

    PacketAssembler asm_;  // continuous ChSequence across both messages
    std::vector<Bunch> all = FragmentMessage(asm_, hello);
    for (const Bunch& b : FragmentMessage(asm_, netspeed)) {
        all.push_back(b);
    }

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    for (const Bunch& b : all) {
        re.OnBunch(b);
    }

    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], hello);
    EXPECT_EQ(got[1], netspeed);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
