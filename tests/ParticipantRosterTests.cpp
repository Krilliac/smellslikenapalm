#include "TestFramework.h"

#include <cstdint>
#include <optional>

#include "Game/ParticipantRoster.h"
#include "Network/WeaponCombatReplication.h"

static_assert(ParticipantActorChannelMap::kLastChannel <
                  WeaponCombatRepl::kM61VisualFirstChannel,
              "participant and projectile channels must remain disjoint");

TEST(ParticipantActorChannels, TaggedIdsNeverAliasAndDuplicateEnsureIsStable) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(7);
    const ParticipantId bot = ParticipantId::Bot(7);

    const ParticipantActorChannelBinding* humanBinding = channels.Ensure(human);
    ASSERT_TRUE(humanBinding != nullptr);
    const std::uint16_t humanPri = humanBinding->priChannel;
    const std::int32_t humanWireId = humanBinding->wirePlayerId;

    const ParticipantActorChannelBinding* duplicate = channels.Ensure(human);
    ASSERT_TRUE(duplicate != nullptr);
    EXPECT_EQ(duplicate->priChannel, humanPri);

    const ParticipantActorChannelBinding* botBinding = channels.Ensure(bot);
    ASSERT_TRUE(botBinding != nullptr);
    EXPECT_NE(botBinding->priChannel, humanPri);
    EXPECT_NE(botBinding->wirePlayerId, humanWireId);
    EXPECT_EQ(channels.Size(), static_cast<std::size_t>(2));
}

TEST(ParticipantActorChannels, RejectsMalformedAndUnencodableParticipants) {
    ParticipantActorChannelMap channels;
    EXPECT_TRUE(channels.Ensure(ParticipantId{}) == nullptr);
    EXPECT_TRUE(channels.Ensure(ParticipantId{
        static_cast<ParticipantKind>(255), 1}) == nullptr);
    EXPECT_TRUE(channels.Ensure(ParticipantId::Human(0)) == nullptr);
    EXPECT_TRUE(channels.Ensure(ParticipantId::Bot(
        ParticipantActorChannelMap::kMaximumTaggedValue + 1u)) == nullptr);
    EXPECT_EQ(channels.Size(), static_cast<std::size_t>(0));
}

TEST(ParticipantActorChannels, ExhaustionFailsClosedInsideRetailChannelBound) {
    ParticipantActorChannelMap channels;
    for (std::size_t i = 0; i < ParticipantActorChannelMap::kSlotCount; ++i) {
        const ParticipantActorChannelBinding* binding = channels.Ensure(
            ParticipantId::Bot(static_cast<std::uint32_t>(i + 1u)));
        ASSERT_TRUE(binding != nullptr);
        EXPECT_TRUE(binding->priChannel >=
                    ParticipantActorChannelMap::kFirstChannel);
        EXPECT_TRUE(binding->pawnChannel <=
                    ParticipantActorChannelMap::kLastChannel);
        EXPECT_TRUE(binding->pawnChannel < 1024u);
    }
    EXPECT_TRUE(channels.Ensure(ParticipantId::Human(900)) == nullptr);
    EXPECT_EQ(channels.Size(), ParticipantActorChannelMap::kSlotCount);
}

TEST(ParticipantActorChannels, RetirementQuarantinesPairUntilConnectionReset) {
    ParticipantActorChannelMap channels;
    const ParticipantId first = ParticipantId::Human(1);
    const auto* firstBinding = channels.Ensure(first);
    ASSERT_TRUE(firstBinding != nullptr);
    const std::uint16_t firstPri = firstBinding->priChannel;

    EXPECT_TRUE(channels.Retire(first));
    EXPECT_TRUE(channels.Find(first) == nullptr);
    EXPECT_EQ(channels.RetiredCount(), static_cast<std::size_t>(1));

    const auto* secondBinding = channels.Ensure(ParticipantId::Human(2));
    ASSERT_TRUE(secondBinding != nullptr);
    EXPECT_NE(secondBinding->priChannel, firstPri);

    channels.Clear();
    EXPECT_EQ(channels.RetiredCount(), static_cast<std::size_t>(0));
    const auto* afterReconnect = channels.Ensure(ParticipantId::Human(3));
    ASSERT_TRUE(afterReconnect != nullptr);
    EXPECT_EQ(afterReconnect->priChannel, firstPri);
}

TEST(ParticipantActorChannels, MapTravelRetiresEveryPairWithoutReuse) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(11);
    const ParticipantId bot = ParticipantId::Bot(11);
    const auto* humanBinding = channels.Ensure(human);
    const auto* botBinding = channels.Ensure(bot);
    ASSERT_TRUE(humanBinding != nullptr);
    ASSERT_TRUE(botBinding != nullptr);
    const std::uint16_t firstPri = humanBinding->priChannel;

    ASSERT_TRUE(channels.MarkPriOpen(human));
    ASSERT_TRUE(channels.MarkPawnOpen(human, humanBinding->pawnGeneration));
    ASSERT_TRUE(channels.MarkPriOpen(bot));
    EXPECT_EQ(channels.RetireAll(), static_cast<std::size_t>(2));
    EXPECT_EQ(channels.Size(), static_cast<std::size_t>(0));
    EXPECT_EQ(channels.RetiredCount(), static_cast<std::size_t>(2));
    EXPECT_TRUE(channels.Find(human) == nullptr);
    EXPECT_TRUE(channels.Find(bot) == nullptr);
    EXPECT_EQ(channels.RetireAll(), static_cast<std::size_t>(0));

    const auto* sameConnection = channels.Ensure(ParticipantId::Bot(12));
    ASSERT_TRUE(sameConnection != nullptr);
    EXPECT_NE(sameConnection->priChannel, firstPri);
    EXPECT_EQ(sameConnection->priChannel,
              static_cast<std::uint16_t>(firstPri + 4u));

    channels.Clear();
    const auto* freshConnection = channels.Ensure(ParticipantId::Bot(13));
    ASSERT_TRUE(freshConnection != nullptr);
    EXPECT_EQ(freshConnection->priChannel, firstPri);
}

TEST(ParticipantActorChannels, PawnGenerationRejectsStaleRespawnOpen) {
    ParticipantActorChannelMap channels;
    const ParticipantId bot = ParticipantId::Bot(42);
    const auto* binding = channels.Ensure(bot);
    ASSERT_TRUE(binding != nullptr);
    const std::uint32_t generationOne = binding->pawnGeneration;

    EXPECT_TRUE(channels.MarkPriOpen(bot));
    EXPECT_TRUE(channels.MarkPawnOpen(bot, generationOne));
    EXPECT_FALSE(channels.BeginPawnIncarnation(bot).has_value());
    EXPECT_TRUE(channels.SetDead(bot, true));
    EXPECT_TRUE(channels.MarkPawnClosing(bot));
    EXPECT_FALSE(channels.MarkPawnClosing(bot));
    EXPECT_FALSE(channels.BeginPawnIncarnation(bot).has_value());
    EXPECT_TRUE(channels.AcknowledgePawnClose(binding->pawnChannel));
    EXPECT_FALSE(channels.AcknowledgePawnClose(binding->pawnChannel));

    const auto generationTwo = channels.BeginPawnIncarnation(bot);
    ASSERT_TRUE(generationTwo.has_value());
    EXPECT_NE(*generationTwo, generationOne);
    EXPECT_FALSE(channels.MarkPawnOpen(bot, generationOne));
    EXPECT_TRUE(channels.MarkPawnOpen(bot, *generationTwo));
    EXPECT_FALSE(channels.Find(bot)->dead);
}

TEST(ParticipantActorChannels, PriCannotAcknowledgePawnClose) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(17);
    const auto* binding = channels.Ensure(human);
    ASSERT_TRUE(binding != nullptr);
    const std::uint16_t priChannel = binding->priChannel;
    const std::uint16_t pawnChannel = binding->pawnChannel;
    const std::uint32_t generation = binding->pawnGeneration;

    ASSERT_TRUE(channels.MarkPawnOpen(human, generation));
    ASSERT_TRUE(channels.MarkPawnClosing(human));
    EXPECT_FALSE(channels.AcknowledgePawnClose(priChannel));
    EXPECT_EQ(channels.Find(human)->pawnState,
              ParticipantActorOpenState::Closing);
    EXPECT_TRUE(channels.AcknowledgePawnClose(pawnChannel));
}

TEST(ParticipantActorChannels, PeerPawnCloseCannotReleaseChannelForReuse) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(18);
    const auto* binding = channels.Ensure(human);
    ASSERT_TRUE(binding != nullptr);
    const std::uint16_t pawnChannel = binding->pawnChannel;
    ASSERT_TRUE(channels.MarkPawnOpen(human, binding->pawnGeneration));

    ASSERT_TRUE(channels.MarkChannelClosed(pawnChannel));
    EXPECT_EQ(channels.Find(human)->pawnState,
              ParticipantActorOpenState::Closing);
    EXPECT_FALSE(channels.BeginPawnIncarnation(human).has_value());
}

TEST(ParticipantActorChannels, ResolverAcceptsOnlyOpenLivingPawnChannel) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(7);
    const ParticipantId bot = ParticipantId::Bot(7);
    const auto* humanBinding = channels.Ensure(human);
    const auto* botBinding = channels.Ensure(bot);
    ASSERT_TRUE(humanBinding != nullptr);
    ASSERT_TRUE(botBinding != nullptr);
    const std::uint16_t humanPri = humanBinding->priChannel;
    const std::uint16_t humanPawn = humanBinding->pawnChannel;
    const std::uint32_t humanGeneration = humanBinding->pawnGeneration;
    const std::uint16_t botPawn = botBinding->pawnChannel;
    const std::uint32_t botGeneration = botBinding->pawnGeneration;

    EXPECT_FALSE(channels.ResolveOpenLivingPawn(humanPri).has_value());
    EXPECT_FALSE(channels.ResolveOpenLivingPawn(humanPawn).has_value());
    ASSERT_TRUE(channels.MarkPawnOpen(human, humanGeneration));
    ASSERT_TRUE(channels.MarkPawnOpen(bot, botGeneration));
    EXPECT_EQ(channels.ResolveOpenLivingPawn(humanPawn),
              std::optional<ParticipantId>(human));
    EXPECT_EQ(channels.ResolveOpenLivingPawn(botPawn),
              std::optional<ParticipantId>(bot));
    EXPECT_NE(channels.ResolveOpenLivingPawn(humanPawn),
              channels.ResolveOpenLivingPawn(botPawn));

    ASSERT_TRUE(channels.SetDead(human, true));
    EXPECT_TRUE(channels.SetDead(human, true));
    EXPECT_FALSE(channels.ResolveOpenLivingPawn(humanPawn).has_value());
    ASSERT_TRUE(channels.MarkPawnClosing(bot));
    EXPECT_FALSE(channels.ResolveOpenLivingPawn(botPawn).has_value());
    ASSERT_TRUE(channels.AcknowledgePawnClose(botPawn));
    EXPECT_FALSE(channels.ResolveOpenLivingPawn(botPawn).has_value());
    EXPECT_FALSE(channels.ResolveOpenLivingPawn(900).has_value());
}

TEST(ParticipantActorChannels, PawnReliableSequenceSurvivesReincarnation) {
    ParticipantActorChannelMap channels;
    const ParticipantId bot = ParticipantId::Bot(8);
    const auto* binding = channels.Ensure(bot);
    ASSERT_TRUE(binding != nullptr);
    const std::uint16_t pawnChannel = binding->pawnChannel;
    const std::uint32_t firstGeneration = binding->pawnGeneration;

    EXPECT_EQ(channels.NextPawnReliableSequence(bot),
              std::optional<std::uint32_t>(1u));
    ASSERT_TRUE(channels.MarkPawnOpen(bot, firstGeneration));
    EXPECT_EQ(channels.NextPawnReliableSequence(bot),
              std::optional<std::uint32_t>(2u));
    ASSERT_TRUE(channels.MarkPawnClosing(bot));
    ASSERT_TRUE(channels.AcknowledgePawnClose(pawnChannel));
    const auto secondGeneration = channels.BeginPawnIncarnation(bot);
    ASSERT_TRUE(secondGeneration.has_value());
    EXPECT_EQ(channels.NextPawnReliableSequence(bot),
              std::optional<std::uint32_t>(3u));
    EXPECT_TRUE(channels.MarkPawnOpen(bot, *secondGeneration));
}

TEST(ParticipantActorChannels, ReliableSequenceWrapsInsideTenBitSpace) {
    ParticipantActorChannelMap channels;
    const ParticipantId human = ParticipantId::Human(9);
    ASSERT_TRUE(channels.Ensure(human) != nullptr);

    for (std::uint32_t expected = 1;
         expected < ParticipantActorChannelMap::kReliableSequenceLimit;
         ++expected) {
        const auto sequence = channels.NextPriReliableSequence(human);
        ASSERT_TRUE(sequence.has_value());
        EXPECT_EQ(*sequence, expected);
    }
    const auto wrapped = channels.NextPriReliableSequence(human);
    ASSERT_TRUE(wrapped.has_value());
    EXPECT_EQ(*wrapped, 0u);
    const auto afterWrap = channels.NextPriReliableSequence(human);
    ASSERT_TRUE(afterWrap.has_value());
    EXPECT_EQ(*afterWrap, 1u);
}

RS2V_TEST_MAIN()
