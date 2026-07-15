#include "TestFramework.h"

#include "Network/ActorReliableSequencer.h"
#include "Network/PacketCodec.h"

#include <cstdint>
#include <vector>

using PacketCodec::ActorReliableSequenceStatus;
using PacketCodec::ActorReliableSequencer;
using PacketCodec::Bunch;

namespace {

Bunch MakeReliable(uint32_t channel, uint32_t sequence, uint8_t marker = 0) {
    Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = channel;
    bunch.chSequence = sequence;
    bunch.payload = {marker};
    bunch.payloadBits = 8;
    return bunch;
}

Bunch MakeReliablePayload(uint32_t channel, uint32_t sequence,
                          size_t payloadBytes, uint8_t marker = 0) {
    Bunch bunch = MakeReliable(channel, sequence, marker);
    bunch.payload.assign(payloadBytes, marker);
    bunch.payloadBits = static_cast<uint32_t>(payloadBytes * 8u);
    return bunch;
}

int StatusValue(ActorReliableSequenceStatus status) {
    return static_cast<int>(status);
}

void ExpectStatus(const PacketCodec::ActorReliableSequenceResult& result,
                  ActorReliableSequenceStatus expected) {
    EXPECT_EQ(StatusValue(result.status), StatusValue(expected));
}

} // namespace

TEST(ActorReliableSequencer, ReleasesInOrderBunchesImmediately) {
    ActorReliableSequencer sequencer;

    const auto first = sequencer.Push(MakeReliable(7, 1, 0x11));
    ExpectStatus(first, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(first.released.size(), 1u);
    EXPECT_EQ(first.released[0].chSequence, 1u);
    EXPECT_EQ(first.released[0].payload[0], 0x11u);

    const auto second = sequencer.Push(MakeReliable(7, 2, 0x22));
    ExpectStatus(second, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(second.released.size(), 1u);
    EXPECT_EQ(second.released[0].chSequence, 2u);
    EXPECT_EQ(sequencer.NextSequence(7), 3u);
}

TEST(ActorReliableSequencer, SuppressesBufferedAndReleasedDuplicates) {
    ActorReliableSequencer sequencer;

    ExpectStatus(sequencer.Push(MakeReliable(9, 2)),
                 ActorReliableSequenceStatus::Buffered);
    ExpectStatus(sequencer.Push(MakeReliable(9, 2, 0xFF)),
                 ActorReliableSequenceStatus::Duplicate);

    const auto released = sequencer.Push(MakeReliable(9, 1));
    ExpectStatus(released, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(released.released.size(), 2u);
    ExpectStatus(sequencer.Push(MakeReliable(9, 1)),
                 ActorReliableSequenceStatus::Duplicate);
    ExpectStatus(sequencer.Push(MakeReliable(9, 2)),
                 ActorReliableSequenceStatus::Duplicate);
}

TEST(ActorReliableSequencer, BuffersTwoBeforeOneThenReleasesContiguously) {
    ActorReliableSequencer sequencer;

    const auto early = sequencer.Push(MakeReliable(12, 2, 0x22));
    ExpectStatus(early, ActorReliableSequenceStatus::Buffered);
    EXPECT_TRUE(early.released.empty());
    EXPECT_EQ(sequencer.PendingBunchCount(12), 1u);

    const auto closed = sequencer.Push(MakeReliable(12, 1, 0x11));
    ExpectStatus(closed, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(closed.released.size(), 2u);
    EXPECT_EQ(closed.released[0].chSequence, 1u);
    EXPECT_EQ(closed.released[1].chSequence, 2u);
    EXPECT_EQ(sequencer.PendingBunchCount(12), 0u);
}

TEST(ActorReliableSequencer, RetainsLaterBunchAcrossARealGap) {
    ActorReliableSequencer sequencer;

    ExpectStatus(sequencer.Push(MakeReliable(13, 3)),
                 ActorReliableSequenceStatus::Buffered);
    const auto first = sequencer.Push(MakeReliable(13, 1));
    ASSERT_EQ(first.released.size(), 1u);
    EXPECT_EQ(first.released[0].chSequence, 1u);
    EXPECT_EQ(sequencer.PendingBunchCount(13), 1u);

    const auto closesGap = sequencer.Push(MakeReliable(13, 2));
    ASSERT_EQ(closesGap.released.size(), 2u);
    EXPECT_EQ(closesGap.released[0].chSequence, 2u);
    EXPECT_EQ(closesGap.released[1].chSequence, 3u);
}

TEST(ActorReliableSequencer, RejectsForwardGapBeyondConfiguredBound) {
    ActorReliableSequencer sequencer(4);

    for (uint32_t sequence = 2; sequence <= 5; ++sequence) {
        ExpectStatus(sequencer.Push(MakeReliable(14, sequence)),
                     ActorReliableSequenceStatus::Buffered);
    }
    ExpectStatus(sequencer.Push(MakeReliable(14, 6)),
                 ActorReliableSequenceStatus::GapOverflow);
    ExpectStatus(sequencer.Push(MakeReliable(14, 400)),
                 ActorReliableSequenceStatus::GapOverflow);
    EXPECT_EQ(sequencer.PendingBunchCount(14), 4u);

    const auto released = sequencer.Push(MakeReliable(14, 1));
    ASSERT_EQ(released.released.size(), 5u);
    EXPECT_EQ(sequencer.NextSequence(14), 6u);
    EXPECT_EQ(sequencer.PendingBunchCount(14), 0u);
}

TEST(ActorReliableSequencer, EnforcesConnectionWidePendingBunchLimit) {
    ActorReliableSequencer sequencer(8, 1, 2, 100);

    ExpectStatus(sequencer.Push(MakeReliable(60, 2, 0x22)),
                 ActorReliableSequenceStatus::Buffered);
    ExpectStatus(sequencer.Push(MakeReliable(61, 2, 0x32)),
                 ActorReliableSequenceStatus::Buffered);
    ExpectStatus(sequencer.Push(MakeReliable(62, 2, 0x42)),
                 ActorReliableSequenceStatus::CapacityExceeded);
    EXPECT_EQ(sequencer.PendingBunchCount(), 2u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 2u);

    // Releasing one channel frees both its bunch slot and payload bytes.
    const auto released = sequencer.Push(MakeReliable(60, 1, 0x21));
    ASSERT_EQ(released.released.size(), 2u);
    EXPECT_EQ(sequencer.PendingBunchCount(), 1u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 1u);
    ExpectStatus(sequencer.Push(MakeReliable(62, 2, 0x42)),
                 ActorReliableSequenceStatus::Buffered);
}

TEST(ActorReliableSequencer, EnforcesConnectionWidePendingPayloadLimit) {
    ActorReliableSequencer sequencer(8, 1, 8, 5);

    ExpectStatus(sequencer.Push(MakeReliablePayload(63, 2, 3, 0xA3)),
                 ActorReliableSequenceStatus::Buffered);
    ExpectStatus(sequencer.Push(MakeReliablePayload(64, 2, 2, 0xB2)),
                 ActorReliableSequenceStatus::Buffered);
    ExpectStatus(sequencer.Push(MakeReliablePayload(65, 2, 1, 0xC1)),
                 ActorReliableSequenceStatus::CapacityExceeded);
    EXPECT_EQ(sequencer.PendingBunchCount(), 2u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 5u);

    // Resetting one channel must return exactly its retained byte allowance.
    EXPECT_TRUE(sequencer.ResetChannel(63));
    EXPECT_EQ(sequencer.PendingBunchCount(), 1u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 2u);
    ExpectStatus(sequencer.Push(MakeReliablePayload(65, 2, 3, 0xC3)),
                 ActorReliableSequenceStatus::Buffered);

    sequencer.Clear();
    EXPECT_EQ(sequencer.PendingBunchCount(), 0u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 0u);
}

TEST(ActorReliableSequencer, ClassifiesOldUntrackedSequenceAsStale) {
    ActorReliableSequencer sequencer(2);
    for (uint32_t sequence = 1; sequence <= 6; ++sequence) {
        sequencer.Push(MakeReliable(15, sequence));
    }

    // Sequence 1 has fallen out of the three-entry duplicate history but is
    // still unambiguously behind the current cursor in modulo space.
    ExpectStatus(sequencer.Push(MakeReliable(15, 1)),
                 ActorReliableSequenceStatus::Stale);
}

TEST(ActorReliableSequencer, KeepsChannelOrderingIndependent) {
    ActorReliableSequencer sequencer;

    ExpectStatus(sequencer.Push(MakeReliable(20, 2)),
                 ActorReliableSequenceStatus::Buffered);
    const auto other = sequencer.Push(MakeReliable(21, 1));
    ASSERT_EQ(other.released.size(), 1u);
    EXPECT_EQ(other.released[0].chIndex, 21u);
    EXPECT_EQ(sequencer.NextSequence(20), 1u);
    EXPECT_EQ(sequencer.NextSequence(21), 2u);

    const auto first = sequencer.Push(MakeReliable(20, 1));
    ASSERT_EQ(first.released.size(), 2u);
    EXPECT_EQ(first.released[0].chIndex, 20u);
    EXPECT_EQ(first.released[1].chIndex, 20u);
}

TEST(ActorReliableSequencer, ClearAndChannelResetDiscardPendingState) {
    ActorReliableSequencer sequencer;
    sequencer.Push(MakeReliable(30, 2));
    sequencer.Push(MakeReliable(31, 2));
    EXPECT_EQ(sequencer.PendingBunchCount(), 2u);

    EXPECT_TRUE(sequencer.ResetChannel(30, 7));
    EXPECT_EQ(sequencer.PendingBunchCount(30), 0u);
    EXPECT_EQ(sequencer.NextSequence(30), 7u);
    ExpectStatus(sequencer.Push(MakeReliable(30, 7)),
                 ActorReliableSequenceStatus::Released);

    sequencer.Clear();
    EXPECT_EQ(sequencer.ChannelCount(), 0u);
    EXPECT_EQ(sequencer.PendingBunchCount(), 0u);
    EXPECT_EQ(sequencer.NextSequence(30), 1u);
}

TEST(ActorReliableSequencer, OrdersAcrossSequenceWrapAround) {
    ActorReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.ResetChannel(40, PacketCodec::kMaxChSequence - 1u));

    ExpectStatus(sequencer.Push(MakeReliable(40, 0, 0x00)),
                 ActorReliableSequenceStatus::Buffered);
    EXPECT_EQ(sequencer.PendingBunchCount(), 1u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 1u);
    const auto wrapped = sequencer.Push(
        MakeReliable(40, PacketCodec::kMaxChSequence - 1u, 0xFF));
    ExpectStatus(wrapped, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(wrapped.released.size(), 2u);
    EXPECT_EQ(wrapped.released[0].chSequence,
              PacketCodec::kMaxChSequence - 1u);
    EXPECT_EQ(wrapped.released[1].chSequence, 0u);
    EXPECT_EQ(sequencer.PendingBunchCount(), 0u);
    EXPECT_EQ(sequencer.PendingPayloadBytes(), 0u);
    EXPECT_EQ(sequencer.NextSequence(40), 1u);
    ExpectStatus(sequencer.Push(MakeReliable(40, 0)),
                 ActorReliableSequenceStatus::Duplicate);
}

TEST(ActorReliableSequencer, ChannelClosePreservesCursorAcrossReuse) {
    ActorReliableSequencer sequencer;

    Bunch close = MakeReliable(41, 1, 0xC1);
    close.bClose = true;
    const auto deliveredClose = sequencer.Push(close);
    ASSERT_EQ(deliveredClose.released.size(), 1u);
    EXPECT_EQ(sequencer.NextSequence(41), 2u);

    // Simulate ConnectionManager closing the incarnation. A speculative next
    // bunch is discarded, but the delivered close remains duplicate evidence.
    ExpectStatus(sequencer.Push(MakeReliable(41, 3, 0x33)),
                 ActorReliableSequenceStatus::Buffered);
    EXPECT_TRUE(sequencer.DiscardPending(41));
    EXPECT_EQ(sequencer.PendingBunchCount(41), 0u);
    EXPECT_EQ(sequencer.NextSequence(41), 2u);
    ExpectStatus(sequencer.Push(close),
                 ActorReliableSequenceStatus::Duplicate);

    // UE3 reuses the channel index without resetting InReliable.
    const auto reused = sequencer.Push(MakeReliable(41, 2, 0x22));
    ExpectStatus(reused, ActorReliableSequenceStatus::Released);
    ASSERT_EQ(reused.released.size(), 1u);
    EXPECT_EQ(sequencer.NextSequence(41), 3u);
}

TEST(ActorReliableSequencer, ExactHalfCycleIsStaleLikeUe3MakeRelative) {
    ActorReliableSequencer sequencer;
    ExpectStatus(
        sequencer.Push(MakeReliable(42, PacketCodec::kMaxChSequence / 2u + 1u)),
        ActorReliableSequenceStatus::Stale);
}

TEST(ActorReliableSequencer, LeavesUnreliableAndInvalidBunchesUntouched) {
    ActorReliableSequencer sequencer;

    Bunch unreliable = MakeReliable(50, 1);
    unreliable.bReliable = false;
    ExpectStatus(sequencer.Push(unreliable),
                 ActorReliableSequenceStatus::Unreliable);
    ExpectStatus(
        sequencer.Push(MakeReliable(static_cast<uint32_t>(kMaxChannels), 1)),
        ActorReliableSequenceStatus::InvalidBunch);
    ExpectStatus(sequencer.Push(MakeReliable(50, PacketCodec::kMaxChSequence)),
                 ActorReliableSequenceStatus::InvalidBunch);
    Bunch invalidPayload = MakeReliable(50, 1);
    invalidPayload.payloadBits = 9;
    ExpectStatus(sequencer.Push(invalidPayload),
                 ActorReliableSequenceStatus::InvalidBunch);
    EXPECT_EQ(sequencer.ChannelCount(), 0u);
}

RS2V_TEST_MAIN()
