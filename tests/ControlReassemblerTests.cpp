// tests/ControlReassemblerTests.cpp
//
// Tests for the inbound control-channel reassembler. The real RS2 client sends
// each control-channel message as ONE reliable bunch (at the production C2S
// MaxPacket of 1280 a single bunch holds up to 10239 data bits, so even the big
// EOS auth-ticket message is not fragmented). So the reassembler's contract is
// PER-BUNCH delivery: each reliable control-channel (chIndex 0) bunch payload is
// delivered as one complete message, in ChSequence order, with duplicates
// ignored. ChSequence is per channel: UE3 tracks InReliable[ChIndex] and buffers
// every successor until the missing reliable bunch arrives. The 10-bit wire value
// wraps from 1023 to 0 and is ordered relative to that per-channel cursor.

#include "TestFramework.h"

#include "Network/ControlReassembler.h"
#include "Network/PacketCodec.h"
#include "Network/ControlChannel.h"
#include "Network/NetMessages.h"

#include <vector>
#include <cstdint>
#include <stdexcept>

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

void AdvanceToFinalSequence(ControlReassembler& re) {
    for (uint32_t seq = 1u; seq < PacketCodec::kMaxChSequence - 1u; ++seq) {
        re.OnBunch(MakeControlBunch(
            seq, {static_cast<uint8_t>(seq & 0xffu)}));
    }
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

// UE3's InReliable cursor is per channel. Later ch0 bunches cannot prove that a
// missing sequence belonged to another channel, so they remain buffered until
// the delayed reliable predecessor arrives.
TEST(ControlReassembler, WaitsForMissingPerChannelSequence) {
    const std::vector<uint8_t> m1 = {0x00, 0x11};
    const std::vector<uint8_t> m3 = {0x03, 0x33};
    const std::vector<uint8_t> m4 = {0x04, 0x44};
    const std::vector<uint8_t> m5 = {0x05, 0x55};
    const std::vector<uint8_t> m6 = {0x06, 0x66};

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });

    re.OnBunch(MakeControlBunch(1, m1));  // delivered immediately; m_nextSeq -> 2
    ASSERT_EQ(got.size(), 1u);

    // Buffer four successors behind seq 2. A pending-count heuristic must never
    // skip the missing reliable message and deliver these out of order.
    re.OnBunch(MakeControlBunch(3, m3));
    re.OnBunch(MakeControlBunch(4, m4));
    re.OnBunch(MakeControlBunch(5, m5));
    re.OnBunch(MakeControlBunch(6, m6));
    EXPECT_EQ(got.size(), 1u);
    EXPECT_EQ(re.PendingBunchCount(), 4u);

    const std::vector<uint8_t> m2 = {0x02, 0x22};
    re.OnBunch(MakeControlBunch(2, m2));

    ASSERT_EQ(got.size(), 6u);
    EXPECT_EQ(got[0], m1);
    EXPECT_EQ(got[1], m2);
    EXPECT_EQ(got[2], m3);
    EXPECT_EQ(got[3], m4);
    EXPECT_EQ(got[4], m5);
    EXPECT_EQ(got[5], m6);
    EXPECT_EQ(re.PendingBunchCount(), 0u);
    EXPECT_EQ(re.NextSequence(), 7u);
}

TEST(ControlReassembler, DeliversAFullSequenceLapAcrossWrap) {
    size_t deliveries = 0u;
    ControlReassembler re(
        [&](const std::vector<uint8_t>&) { ++deliveries; });

    for (uint32_t seq = 1u; seq < PacketCodec::kMaxChSequence; ++seq) {
        re.OnBunch(MakeControlBunch(seq, {0x11u}));
    }
    re.OnBunch(MakeControlBunch(0u, {0x22u}));
    re.OnBunch(MakeControlBunch(1u, {0x33u}));

    EXPECT_EQ(deliveries,
              static_cast<size_t>(PacketCodec::kMaxChSequence) + 1u);
    EXPECT_EQ(re.NextSequence(), 2u);
    EXPECT_LT(re.NextSequence(), PacketCodec::kMaxChSequence);
}

TEST(ControlReassembler, OrdersAndDeduplicatesAcrossWrap) {
    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re(
        [&](const std::vector<uint8_t>& message) { got.push_back(message); });
    AdvanceToFinalSequence(re);
    ASSERT_EQ(re.NextSequence(), PacketCodec::kMaxChSequence - 1u);
    got.clear();

    // The first wrapped successor is buffered. A differing retransmit keeps the
    // first copy, then seq1023 releases both in modular order.
    re.OnBunch(MakeControlBunch(0u, {0xa0u}));
    re.OnBunch(MakeControlBunch(0u, {0xb0u}));
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(re.PendingBunchCount(), 1u);
    re.OnBunch(MakeControlBunch(
        PacketCodec::kMaxChSequence - 1u, {0xffu}));

    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], (std::vector<uint8_t>{0xffu}));
    EXPECT_EQ(got[1], (std::vector<uint8_t>{0xa0u}));
    EXPECT_EQ(re.NextSequence(), 1u);

    // A delayed predecessor from just behind the wrapped cursor is stale, while
    // the newly valid seq1 from the next lap is accepted.
    re.OnBunch(MakeControlBunch(
        PacketCodec::kMaxChSequence - 1u, {0xeeu}));
    re.OnBunch(MakeControlBunch(1u, {0x11u}));
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[2], (std::vector<uint8_t>{0x11u}));
    EXPECT_EQ(re.NextSequence(), 2u);
}

TEST(ControlReassembler, EnforcesModularWindowAndWireDomainAtWrap) {
    EXPECT_EQ(PacketCodec::AdvanceChSequence(PacketCodec::kMaxChSequence),
              PacketCodec::kMaxChSequence);
    ControlReassembler re([](const std::vector<uint8_t>&) {});
    AdvanceToFinalSequence(re);
    ASSERT_EQ(re.NextSequence(), PacketCodec::kMaxChSequence - 1u);

    // From 1023, raw 126 is +127 and legal; 127 is +128 and outside the
    // RELIABLE_BUFFER window. 1022 is stale, 510 is exactly half a cycle from
    // the last retired sequence and therefore behind,
    // and 1024 is not a wire-representable ChSequence.
    re.OnBunch(MakeControlBunch(126u, {0x7fu}));
    re.OnBunch(MakeControlBunch(127u, {0x80u}));
    re.OnBunch(MakeControlBunch(1022u, {0xfeu}));
    re.OnBunch(MakeControlBunch(510u, {0x00u}));
    re.OnBunch(MakeControlBunch(PacketCodec::kMaxChSequence, {0xbau}));

    EXPECT_EQ(re.PendingBunchCount(), 1u);
    EXPECT_EQ(re.NextSequence(), PacketCodec::kMaxChSequence - 1u);
}

TEST(ControlReassembler, CallbackReentryQueuesWithoutRedelivery) {
    std::vector<uint8_t> delivered;
    ControlReassembler* reentrant = nullptr;
    ControlReassembler re([&](const std::vector<uint8_t>& message) {
        ASSERT_FALSE(message.empty());
        delivered.push_back(message[0]);
        if (message[0] == 0x01u) {
            reentrant->OnBunch(MakeControlBunch(2u, {0x02u}));
        }
    });
    reentrant = &re;

    re.OnBunch(MakeControlBunch(1u, {0x01u}));

    EXPECT_EQ(delivered, (std::vector<uint8_t>{0x01u, 0x02u}));
    EXPECT_EQ(re.PendingBunchCount(), 0u);
    EXPECT_EQ(re.NextSequence(), 3u);
}

TEST(ControlReassembler, ReentrantBurstRemainsInsidePendingCap) {
    std::vector<uint8_t> delivered;
    ControlReassembler* reentrant = nullptr;
    ControlReassembler re([&](const std::vector<uint8_t>& message) {
        ASSERT_FALSE(message.empty());
        delivered.push_back(message[0]);
        if (message[0] == 0x01u) {
            for (uint32_t seq = 2u; seq <= 200u; ++seq) {
                reentrant->OnBunch(MakeControlBunch(
                    seq, {static_cast<uint8_t>(seq)}));
                EXPECT_LE(reentrant->PendingBunchCount(),
                          ControlReassembler::kMaximumPendingBunches);
            }
            // The active seq1 plus its 127 legal successors occupy the same
            // bounded pending map. There is no unaccounted dispatch FIFO.
            EXPECT_EQ(reentrant->PendingBunchCount(),
                      ControlReassembler::kMaximumPendingBunches);
        }
    });
    reentrant = &re;

    re.OnBunch(MakeControlBunch(1u, {0x01u}));

    ASSERT_EQ(delivered.size(),
              ControlReassembler::kMaximumPendingBunches);
    for (size_t index = 0u; index < delivered.size(); ++index) {
        EXPECT_EQ(delivered[index], static_cast<uint8_t>(index + 1u));
    }
    EXPECT_EQ(re.PendingBunchCount(), 0u);
    EXPECT_EQ(re.NextSequence(), 129u);
}

TEST(ControlReassembler, ThrowingCallbackLeavesCurrentSequenceRetryable) {
    std::vector<uint8_t> delivered;
    size_t firstMessageAttempts = 0u;
    bool failFirstAttempt = true;
    ControlReassembler* reentrant = nullptr;
    ControlReassembler re([&](const std::vector<uint8_t>& message) {
        ASSERT_FALSE(message.empty());
        if (message[0] == 0x01u) {
            ++firstMessageAttempts;
            if (failFirstAttempt) {
                failFirstAttempt = false;
                reentrant->OnBunch(MakeControlBunch(2u, {0x02u}));
                throw std::runtime_error("retry control callback");
            }
        }
        delivered.push_back(message[0]);
    });
    reentrant = &re;

    EXPECT_THROW(re.OnBunch(MakeControlBunch(1u, {0x01u})),
                 std::runtime_error);
    EXPECT_TRUE(delivered.empty());
    EXPECT_EQ(firstMessageAttempts, 1u);
    EXPECT_EQ(re.PendingBunchCount(), 2u);
    EXPECT_EQ(re.NextSequence(), 1u);

    // The retransmit keeps the first buffered copy, retries seq1, then releases
    // the reentrantly buffered seq2 in order.
    EXPECT_NO_THROW(re.OnBunch(MakeControlBunch(1u, {0x91u})));
    EXPECT_EQ(firstMessageAttempts, 2u);
    EXPECT_EQ(delivered, (std::vector<uint8_t>{0x01u, 0x02u}));
    EXPECT_EQ(re.PendingBunchCount(), 0u);
    EXPECT_EQ(re.NextSequence(), 3u);
}

RS2V_TEST_MAIN()
