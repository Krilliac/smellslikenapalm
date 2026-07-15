#include "TestFramework.h"

#include "Network/OutboundReliableSequencer.h"

#include <cstddef>
#include <cstdint>
#include <limits>

using PacketCodec::OutboundReliableSequenceError;
using PacketCodec::OutboundReliableSequencer;

namespace {

template <typename T>
void ExpectError(
    const std::expected<T, OutboundReliableSequenceError>& result,
    OutboundReliableSequenceError expected) {
    EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        EXPECT_EQ(result.error(), expected);
    }
}

} // namespace

TEST(OutboundReliableSequencer, StartsUninitializedAndRejectsOperations) {
    OutboundReliableSequencer sequencer;

    EXPECT_FALSE(sequencer.IsInitialized());
    EXPECT_FALSE(sequencer.NextSequence().has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
    EXPECT_EQ(sequencer.IssuanceWindowSize(), 0u);
    EXPECT_EQ(sequencer.AvailableCapacity(),
              OutboundReliableSequencer::kMaximumOutstanding);
    ExpectError(sequencer.ReserveBatch(1u),
                OutboundReliableSequenceError::Uninitialized);
    ExpectError(sequencer.Release(0u),
                OutboundReliableSequenceError::Uninitialized);
}

TEST(OutboundReliableSequencer, SeedReservesContiguouslyAcrossModuloWrap) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(PacketCodec::kMaxChSequence - 2u).has_value());

    const auto reservation = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(reservation.has_value());
    ASSERT_EQ(reservation->size(), 3u);
    EXPECT_EQ((*reservation)[0], PacketCodec::kMaxChSequence - 1u);
    EXPECT_EQ((*reservation)[1], 0u);
    EXPECT_EQ((*reservation)[2], 1u);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 2u);
    EXPECT_EQ(sequencer.OutstandingCount(), 3u);
    EXPECT_TRUE(sequencer.IsInFlight(PacketCodec::kMaxChSequence - 1u));
    EXPECT_TRUE(sequencer.IsInFlight(0u));
    EXPECT_TRUE(sequencer.IsInFlight(1u));
}

TEST(OutboundReliableSequencer, RejectsInvalidSeedAndReseedingWithoutMutation) {
    OutboundReliableSequencer sequencer;

    ExpectError(sequencer.Seed(PacketCodec::kMaxChSequence),
                OutboundReliableSequenceError::InvalidSequence);
    EXPECT_FALSE(sequencer.IsInitialized());

    ASSERT_TRUE(sequencer.Seed(7u).has_value());
    ExpectError(sequencer.Seed(9u),
                OutboundReliableSequenceError::AlreadyInitialized);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 8u);
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
}

TEST(OutboundReliableSequencer, AdoptsExternalBootstrapAndValidatesDuplicates) {
    OutboundReliableSequencer sequencer;

    ASSERT_TRUE(
        sequencer.Adopt(PacketCodec::kMaxChSequence - 1u).has_value());
    EXPECT_TRUE(sequencer.IsInitialized());
    EXPECT_TRUE(
        sequencer.IsInFlight(PacketCodec::kMaxChSequence - 1u));
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 0u);

    ASSERT_TRUE(sequencer.Adopt(0u).has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 1u);
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);

    ExpectError(sequencer.Adopt(0u),
                OutboundReliableSequenceError::SequenceInFlight);
    ExpectError(sequencer.Adopt(2u),
                OutboundReliableSequenceError::NonContiguousAdoption);
    ExpectError(sequencer.Adopt(PacketCodec::kMaxChSequence),
                OutboundReliableSequenceError::InvalidSequence);
    EXPECT_EQ(*sequencer.NextSequence(), 1u);
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);
}

TEST(OutboundReliableSequencer, EnforcesUe3ReliableBufferLimitAtomically) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(0u).has_value());

    ExpectError(sequencer.ReserveBatch(
                    OutboundReliableSequencer::kMaximumOutstanding + 1u),
                OutboundReliableSequenceError::OutstandingLimit);
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 1u);

    const auto full =
        sequencer.ReserveBatch(OutboundReliableSequencer::kMaximumOutstanding);
    ASSERT_TRUE(full.has_value());
    ASSERT_EQ(full->size(),
              OutboundReliableSequencer::kMaximumOutstanding);
    EXPECT_EQ(sequencer.OutstandingCount(),
              OutboundReliableSequencer::kMaximumOutstanding);
    EXPECT_EQ(sequencer.AvailableCapacity(), 0u);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    const uint32_t cursorAtLimit = *sequencer.NextSequence();
    ASSERT_TRUE(sequencer.CommitBatch(*full).has_value());

    ExpectError(sequencer.ReserveBatch(1u),
                OutboundReliableSequenceError::OutstandingLimit);
    ExpectError(sequencer.Adopt(cursorAtLimit),
                OutboundReliableSequenceError::OutstandingLimit);
    ExpectError(sequencer.ReserveBatch(
                    std::numeric_limits<size_t>::max()),
                OutboundReliableSequenceError::OutstandingLimit);
    EXPECT_EQ(*sequencer.NextSequence(), cursorAtLimit);
    EXPECT_EQ(sequencer.OutstandingCount(),
              OutboundReliableSequencer::kMaximumOutstanding);

    ASSERT_TRUE(sequencer.Release(full->front()).has_value());
    const auto replacement = sequencer.ReserveBatch(1u);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(sequencer.OutstandingCount(),
              OutboundReliableSequencer::kMaximumOutstanding);
}

TEST(OutboundReliableSequencer, EmptyBatchFailureDoesNotMutateState) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(100u).has_value());

    ExpectError(sequencer.ReserveBatch(0u),
                OutboundReliableSequenceError::InvalidBatchSize);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 101u);
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
}

TEST(OutboundReliableSequencer,
     CapacityPreflightMatchesReservationWithoutMutatingCursor) {
    OutboundReliableSequencer sequencer;
    ExpectError(sequencer.CanReserveBatch(1u),
                OutboundReliableSequenceError::Uninitialized);
    ASSERT_TRUE(sequencer.Seed(100u).has_value());

    const auto nextBefore = sequencer.NextSequence();
    const size_t outstandingBefore = sequencer.OutstandingCount();
    const size_t windowBefore = sequencer.IssuanceWindowSize();
    EXPECT_TRUE(sequencer.CanReserveBatch(3u).has_value());
    EXPECT_EQ(sequencer.NextSequence(), nextBefore);
    EXPECT_EQ(sequencer.OutstandingCount(), outstandingBefore);
    EXPECT_EQ(sequencer.IssuanceWindowSize(), windowBefore);

    const auto reservation = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(reservation->front(), 101u);
    ExpectError(sequencer.CanReserveBatch(1u),
                OutboundReliableSequenceError::UnpublishedReservation);
    ASSERT_TRUE(sequencer.CancelBatch(*reservation).has_value());
    EXPECT_EQ(sequencer.NextSequence(), nextBefore);

    ExpectError(
        sequencer.CanReserveBatch(
            OutboundReliableSequencer::kMaximumOutstanding + 1u),
        OutboundReliableSequenceError::OutstandingLimit);
    EXPECT_EQ(sequencer.NextSequence(), nextBefore);
}

TEST(OutboundReliableSequencer, OldGapCapsForwardIssuanceAtReliableBuffer) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(0u).has_value());

    const auto retained = sequencer.ReserveBatch(1u);
    ASSERT_TRUE(retained.has_value());
    ASSERT_EQ(retained->front(), 1u);
    ASSERT_TRUE(sequencer.CommitBatch(*retained).has_value());

    // ACK successors out of order while retaining sequence 1. They no longer
    // need retransmission, but they must remain in the issuance window until
    // the oldest gap closes or the sender could exceed UE3's ordinary reliable
    // record window at the receiver.
    for (size_t index = 0;
         index < OutboundReliableSequencer::kMaximumOutstanding - 1u;
         ++index) {
        const auto successor = sequencer.ReserveBatch(1u);
        ASSERT_TRUE(successor.has_value());
        ASSERT_TRUE(sequencer.CommitBatch(*successor).has_value());
        ASSERT_TRUE(sequencer.Release(successor->front()).has_value());
    }
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 128u);
    EXPECT_EQ(sequencer.OutstandingCount(), 1u);
    EXPECT_EQ(sequencer.IssuanceWindowSize(),
              OutboundReliableSequencer::kMaximumOutstanding);
    EXPECT_EQ(sequencer.AvailableCapacity(), 0u);

    ExpectError(sequencer.ReserveBatch(1u),
                OutboundReliableSequenceError::OutstandingLimit);
    EXPECT_EQ(*sequencer.NextSequence(), 128u);
    EXPECT_EQ(sequencer.OutstandingCount(), 1u);

    ASSERT_TRUE(sequencer.Release(1u).has_value());
    EXPECT_EQ(sequencer.IssuanceWindowSize(), 0u);
    EXPECT_EQ(sequencer.AvailableCapacity(),
              OutboundReliableSequencer::kMaximumOutstanding);
    const auto resumed = sequencer.ReserveBatch(1u);
    ASSERT_TRUE(resumed.has_value());
    ASSERT_EQ(resumed->size(), 1u);
    EXPECT_EQ(resumed->front(), 128u);
}

TEST(OutboundReliableSequencer, OutOfOrderAcksReclaimOnlyContiguousPrefix) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(9u).has_value());
    const auto batch = sequencer.ReserveBatch(4u);
    ASSERT_TRUE(batch.has_value());
    ASSERT_TRUE(sequencer.CommitBatch(*batch).has_value());

    ASSERT_TRUE(sequencer.Release((*batch)[3]).has_value());
    ASSERT_TRUE(sequencer.Release((*batch)[1]).has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);
    EXPECT_EQ(sequencer.IssuanceWindowSize(), 4u);

    ASSERT_TRUE(sequencer.Release((*batch)[0]).has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 1u);
    EXPECT_EQ(sequencer.IssuanceWindowSize(), 2u);
    ASSERT_TRUE(sequencer.Release((*batch)[2]).has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
    EXPECT_EQ(sequencer.IssuanceWindowSize(), 0u);
}

TEST(OutboundReliableSequencer, CancelsLatestBatchAcrossWrapAndReusesItExactly) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(PacketCodec::kMaxChSequence - 2u).has_value());

    const auto wrapped = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(wrapped.has_value());
    ASSERT_EQ(wrapped->size(), 3u);
    ASSERT_TRUE(sequencer.CancelBatch(*wrapped).has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(),
              PacketCodec::kMaxChSequence - 1u);
    for (const uint32_t sequence : *wrapped) {
        EXPECT_FALSE(sequencer.IsInFlight(sequence));
    }

    const auto reservedAgain = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(reservedAgain.has_value());
    EXPECT_EQ(reservedAgain->SequenceValues(), wrapped->SequenceValues());
}

TEST(OutboundReliableSequencer, RejectsConcurrentUnpublishedMutation) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(0u).has_value());

    const auto first = sequencer.ReserveBatch(2u);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    const uint32_t cursor = *sequencer.NextSequence();

    ExpectError(sequencer.ReserveBatch(2u),
                OutboundReliableSequenceError::UnpublishedReservation);
    ExpectError(sequencer.Adopt(cursor),
                OutboundReliableSequenceError::UnpublishedReservation);
    EXPECT_EQ(*sequencer.NextSequence(), cursor);
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);
    for (const uint32_t sequence : *first) {
        EXPECT_TRUE(sequencer.IsInFlight(sequence));
    }
    ASSERT_TRUE(sequencer.CancelBatch(*first).has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
}

TEST(OutboundReliableSequencer, ReservationTokenRejectsSameShapeAba) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(20u).has_value());
    const auto old = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(old.has_value());
    ASSERT_TRUE(sequencer.CancelBatch(*old).has_value());

    const auto current = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->SequenceValues(), old->SequenceValues());
    ExpectError(sequencer.CancelBatch(*old),
                OutboundReliableSequenceError::NotLatestReservation);
    EXPECT_EQ(sequencer.OutstandingCount(), 3u);
    ASSERT_TRUE(sequencer.CancelBatch(*current).has_value());
}

TEST(OutboundReliableSequencer, ReservationTokenIsBoundToOwningSequencer) {
    OutboundReliableSequencer first;
    OutboundReliableSequencer second;
    ASSERT_TRUE(first.Seed(10u).has_value());
    ASSERT_TRUE(second.Seed(10u).has_value());

    const auto firstReservation = first.ReserveBatch(2u);
    const auto secondReservation = second.ReserveBatch(2u);
    ASSERT_TRUE(firstReservation.has_value());
    ASSERT_TRUE(secondReservation.has_value());
    EXPECT_EQ(firstReservation->SequenceValues(),
              secondReservation->SequenceValues());

    ExpectError(second.CancelBatch(*firstReservation),
                OutboundReliableSequenceError::NotLatestReservation);
    EXPECT_EQ(second.OutstandingCount(), 2u);
    ASSERT_TRUE(second.CancelBatch(*secondReservation).has_value());
    ASSERT_TRUE(first.CancelBatch(*firstReservation).has_value());
}

TEST(OutboundReliableSequencer, ReleaseRejectsUnpublishedReservation) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(30u).has_value());
    const auto latest = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(latest.has_value());
    ExpectError(sequencer.Release((*latest)[1]),
                OutboundReliableSequenceError::UnpublishedReservation);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    const uint32_t cursor = *sequencer.NextSequence();

    EXPECT_EQ(*sequencer.NextSequence(), cursor);
    EXPECT_EQ(sequencer.OutstandingCount(), 3u);
    EXPECT_TRUE(sequencer.IsInFlight((*latest)[0]));
    EXPECT_TRUE(sequencer.IsInFlight((*latest)[1]));
    EXPECT_TRUE(sequencer.IsInFlight((*latest)[2]));
    ASSERT_TRUE(sequencer.CancelBatch(*latest).has_value());
}

TEST(OutboundReliableSequencer, CommitDisablesRollbackWithoutReleasingBatch) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(40u).has_value());
    const auto latest = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(latest.has_value());
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    const uint32_t cursor = *sequencer.NextSequence();

    ASSERT_TRUE(sequencer.CommitBatch(*latest).has_value());
    EXPECT_EQ(*sequencer.NextSequence(), cursor);
    EXPECT_EQ(sequencer.OutstandingCount(), 3u);
    for (const uint32_t sequence : *latest) {
        EXPECT_TRUE(sequencer.IsInFlight(sequence));
    }

    ExpectError(sequencer.CancelBatch(*latest),
                OutboundReliableSequenceError::NotLatestReservation);
    ExpectError(sequencer.CommitBatch(*latest),
                OutboundReliableSequenceError::NotLatestReservation);
    EXPECT_EQ(*sequencer.NextSequence(), cursor);
    EXPECT_EQ(sequencer.OutstandingCount(), 3u);
}

TEST(OutboundReliableSequencer, CommitRejectsStaleTokenWithoutMutation) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(50u).has_value());
    const auto stale = sequencer.ReserveBatch(2u);
    ASSERT_TRUE(stale.has_value());
    ASSERT_TRUE(sequencer.CommitBatch(*stale).has_value());
    const auto latest = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(latest.has_value());
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    const uint32_t cursor = *sequencer.NextSequence();

    ExpectError(sequencer.CommitBatch(*stale),
                OutboundReliableSequenceError::NotLatestReservation);
    EXPECT_EQ(*sequencer.NextSequence(), cursor);
    EXPECT_EQ(sequencer.OutstandingCount(), 5u);
    for (const uint32_t sequence : *stale) {
        EXPECT_TRUE(sequencer.IsInFlight(sequence));
    }
    for (const uint32_t sequence : *latest) {
        EXPECT_TRUE(sequencer.IsInFlight(sequence));
    }
    ASSERT_TRUE(sequencer.CommitBatch(*latest).has_value());
}

TEST(OutboundReliableSequencer, AncientCommittedTokenCannotCancelAfterFullLap) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(0u).has_value());
    const auto ancient = sequencer.ReserveBatch(1u);
    ASSERT_TRUE(ancient.has_value());
    ASSERT_TRUE(sequencer.CommitBatch(*ancient).has_value());
    ASSERT_TRUE(sequencer.Release(ancient->front()).has_value());

    for (size_t index = 0; index < PacketCodec::kMaxChSequence - 1u; ++index) {
        const auto step = sequencer.ReserveBatch(1u);
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(sequencer.CommitBatch(*step).has_value());
        ASSERT_TRUE(sequencer.Release(step->front()).has_value());
    }
    const auto current = sequencer.ReserveBatch(1u);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->front(), ancient->front());
    ExpectError(sequencer.CancelBatch(*ancient),
                OutboundReliableSequenceError::NotLatestReservation);
    ASSERT_TRUE(sequencer.CancelBatch(*current).has_value());
}

TEST(OutboundReliableSequencer, ConnectionResetDropsRollbackMetadata) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(70u).has_value());
    const auto oldReservation = sequencer.ReserveBatch(2u);
    ASSERT_TRUE(oldReservation.has_value());

    sequencer.Clear();
    ASSERT_TRUE(sequencer.Seed(70u).has_value());
    const auto newReservation = sequencer.ReserveBatch(2u);
    ASSERT_TRUE(newReservation.has_value());
    EXPECT_EQ(newReservation->SequenceValues(),
              oldReservation->SequenceValues());
    ExpectError(sequencer.CancelBatch(*oldReservation),
                OutboundReliableSequenceError::NotLatestReservation);
    ExpectError(sequencer.CommitBatch(*oldReservation),
                OutboundReliableSequenceError::NotLatestReservation);
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);
    ASSERT_TRUE(sequencer.NextSequence().has_value());
    EXPECT_EQ(*sequencer.NextSequence(), 73u);
    ASSERT_TRUE(sequencer.CancelBatch(*newReservation).has_value());
}

TEST(OutboundReliableSequencer, ReleaseValidatesAckAndClearDropsAllState) {
    OutboundReliableSequencer sequencer;
    ASSERT_TRUE(sequencer.Seed(10u).has_value());
    const auto reservation = sequencer.ReserveBatch(3u);
    ASSERT_TRUE(reservation.has_value());
    ASSERT_TRUE(sequencer.CommitBatch(*reservation).has_value());

    ASSERT_TRUE(sequencer.Release(12u).has_value());
    EXPECT_FALSE(sequencer.IsInFlight(12u));
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);
    ExpectError(sequencer.Release(12u),
                OutboundReliableSequenceError::NotInFlight);
    ExpectError(sequencer.Release(PacketCodec::kMaxChSequence),
                OutboundReliableSequenceError::InvalidSequence);
    EXPECT_EQ(sequencer.OutstandingCount(), 2u);

    sequencer.Clear();
    EXPECT_FALSE(sequencer.IsInitialized());
    EXPECT_FALSE(sequencer.NextSequence().has_value());
    EXPECT_EQ(sequencer.OutstandingCount(), 0u);
    EXPECT_FALSE(sequencer.IsInFlight(11u));
    EXPECT_EQ(sequencer.AvailableCapacity(),
              OutboundReliableSequencer::kMaximumOutstanding);
}

RS2V_TEST_MAIN()
