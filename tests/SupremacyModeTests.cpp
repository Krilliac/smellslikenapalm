// Focused tests for retail-style Supremacy score flow and objective topology.

#include "TestFramework.h"

#include "Game/GameServer.h"
#include "Game/SupremacyMode.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

class RecordingGameServer final : public GameServer {
public:
    void BroadcastChatMessage(const std::string& message) override {
        chatMessages.push_back(message);
    }

    std::size_t CountChatMessage(const std::string& message) const {
        return static_cast<std::size_t>(
            std::count(chatMessages.begin(), chatMessages.end(), message));
    }

    std::vector<std::string> chatMessages;
};

void ConfigureHueCity(SupremacyMode& mode) {
    mode.ClearObjectives();

    // Retail Hue City objective values and initial home ownership.
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);  // Command Post
    mode.SetObjectiveMetadata(1, 0, 2);                             // Hospital
    mode.SetObjectiveMetadata(2, 0, 2);                             // Governor's House
    mode.SetObjectiveMetadata(3, 0, 1);                             // Ammo Supply Cache
    mode.SetObjectiveMetadata(4, 0, 3);                             // Artillery Position
    mode.SetObjectiveMetadata(5, SupremacyMode::kNorthTeamId, 1);  // Temple

    mode.SetObjectiveLinks({
        {0, {1, 2}},
        {1, {0, 2}},
        {2, {0, 1, 3, 4}},
        {3, {2, 5}},
        {4, {2, 5}},
        {5, {3, 4}},
    });
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 5);
}

void BeginActiveRound(SupremacyMode& mode) {
    mode.StartRound();
    mode.Update(30.0f);
}

}  // namespace

TEST(SupremacyMode, FinishedTransitionBroadcastsMatchCompletionExactlyOnce) {
    RecordingGameServer server;
    SupremacyMode mode(&server);
    mode.Initialize();
    mode.StartRound();
    mode.Update(30.0f);
    ASSERT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);

    mode.EndRound();
    ASSERT_EQ(mode.GetPhase(), SupremacyMode::Phase::PostRound);
    mode.Update(15.0f);

    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Finished);
    EXPECT_EQ(server.CountChatMessage("[Supremacy] Match complete!"), 1u);
}

TEST(SupremacyMode, PreparationDoesNotPermitObjectiveCapture) {
    SupremacyMode mode(nullptr);
    mode.Initialize();
    EXPECT_FALSE(mode.CanCaptureObjectives());

    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Preparation);
    EXPECT_FALSE(mode.CanCaptureObjectives());

    mode.Update(30.0f);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);
    EXPECT_TRUE(mode.CanCaptureObjectives());
}

TEST(SupremacyMode, PhaseClockUsesTheDurationAssignedToEachPhase) {
    SupremacyMode mode(nullptr);
    mode.Initialize();
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 0.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 0.0f);

    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Preparation);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 30.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 30.0f);

    mode.Update(30.0f);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 1200.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 1200.0f);

    mode.OnTicketsDepleted(SupremacyMode::kSouthTeamId);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::SuddenDeath);
    EXPECT_FALSE(mode.CanCaptureObjectives());
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 0.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 0.0f);

    mode.OnTeamEliminated(SupremacyMode::kSouthTeamId);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::PostRound);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 15.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 15.0f);

    mode.Update(15.0f);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Finished);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 0.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 0.0f);
}

TEST(SupremacyMode, HueCityTopologyCountsOnlyOwnedPathsToHQ) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    EXPECT_TRUE(mode.HasObjective(0));
    EXPECT_EQ(mode.GetObjectivePointValue(4), 3);
    ASSERT_TRUE(mode.GetTeamHQ(SupremacyMode::kSouthTeamId).has_value());
    EXPECT_EQ(*mode.GetTeamHQ(SupremacyMode::kSouthTeamId), 0u);

    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 1);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 1);
    EXPECT_EQ(mode.GetSouthConnectedObjectiveValue(), 1);
    EXPECT_EQ(mode.GetNorthConnectedObjectiveValue(), 1);
    EXPECT_FALSE(mode.IsObjectiveLinked(2, SupremacyMode::kSouthTeamId));

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(2, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(4, SupremacyMode::kSouthTeamId);

    EXPECT_TRUE(mode.IsObjectiveLinked(4, SupremacyMode::kSouthTeamId));
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 8);

    // Breaking the Governor's House link disconnects Artillery, while
    // Hospital remains connected directly to the Command Post.
    mode.OnObjectiveCaptured(2, SupremacyMode::kNorthTeamId);
    EXPECT_FALSE(mode.IsObjectiveLinked(4, SupremacyMode::kSouthTeamId));
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 3);
}

TEST(SupremacyMode, SupplyLinesFollowAuthoredEdgesOutwardFromHeadquarters) {
    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);
    mode.SetObjectiveMetadata(1, SupremacyMode::kSouthTeamId, 2);
    mode.SetObjectiveMetadata(2, SupremacyMode::kSouthTeamId, 4);
    mode.SetObjectiveMetadata(3, SupremacyMode::kNorthTeamId, 1);
    mode.SetObjectiveMetadata(9, 0, 1);
    mode.SetObjectiveLinks({
        {0, {1}},  // HQ can reach objective 1.
        {1, {9}},  // Objective 1 has no reverse edge to HQ.
        {2, {0}},  // A reverse-only edge must not connect objective 2.
        {3, {}},
        {9, {}},
    });
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 3);
    BeginActiveRound(mode);

    ASSERT_TRUE(mode.UsesSupplyLines());
    EXPECT_TRUE(mode.IsObjectiveLinked(1, SupremacyMode::kSouthTeamId));
    EXPECT_FALSE(mode.IsObjectiveLinked(2, SupremacyMode::kSouthTeamId));
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 3);
}

TEST(SupremacyMode, OwnedIsolatedObjectiveRemainsConnectedWithSupplyLines) {
    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);
    mode.SetObjectiveMetadata(1, SupremacyMode::kNorthTeamId, 1);
    mode.SetObjectiveMetadata(2, SupremacyMode::kSouthTeamId, 4);
    mode.SetObjectiveLinks({
        {0, {1}},
        {1, {0}},
        {2, {}},
    });
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 1);
    BeginActiveRound(mode);

    ASSERT_TRUE(mode.UsesSupplyLines());
    EXPECT_TRUE(mode.IsObjectiveLinked(2, SupremacyMode::kSouthTeamId));
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 5);

    // Retail's isolated-objective special case does not depend on the home
    // base remaining under that team's control.
    mode.OnObjectiveCaptured(0, SupremacyMode::kNorthTeamId);
    EXPECT_TRUE(mode.IsObjectiveLinked(2, SupremacyMode::kSouthTeamId));
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 4);
}

TEST(SupremacyMode, NoHeadquartersScoresEveryControlledObjective) {
    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 2);
    mode.SetObjectiveMetadata(1, SupremacyMode::kSouthTeamId, 3);
    mode.SetObjectiveMetadata(2, SupremacyMode::kNorthTeamId, 1);
    BeginActiveRound(mode);

    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 5);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 1);
    EXPECT_EQ(mode.GetTeamObjectiveValue(99), 0);
    EXPECT_FALSE(mode.IsObjectiveLinked(0, SupremacyMode::kSouthTeamId));

    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), 4);
}

TEST(SupremacyMode, OneHeadquartersDisablesSupplyLinesForBothTeams) {
    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);
    mode.SetObjectiveMetadata(1, SupremacyMode::kSouthTeamId, 4);
    mode.SetObjectiveMetadata(2, SupremacyMode::kNorthTeamId, 2);
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    BeginActiveRound(mode);

    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 5);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 2);
    EXPECT_FALSE(mode.IsObjectiveLinked(0, SupremacyMode::kSouthTeamId));

    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), 3);
}

TEST(SupremacyMode, MalformedHeadquartersFallsBackToAllControlledObjectives) {
    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);
    mode.SetObjectiveMetadata(1, SupremacyMode::kSouthTeamId, 4);
    mode.SetObjectiveMetadata(2, SupremacyMode::kNorthTeamId, 3);
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 99);
    BeginActiveRound(mode);

    ASSERT_TRUE(mode.GetTeamHQ(SupremacyMode::kNorthTeamId).has_value());
    EXPECT_EQ(*mode.GetTeamHQ(SupremacyMode::kNorthTeamId), 99u);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 5);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 3);
    EXPECT_FALSE(mode.IsObjectiveLinked(0, SupremacyMode::kSouthTeamId));

    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), 2);

    // One retail objective cannot carry both HomeBaseForTeam enum values.
    // Treat the API-created alias as another malformed topology, not as two
    // valid supply-line roots.
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 0);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 5);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 3);
    EXPECT_FALSE(mode.IsObjectiveLinked(0, SupremacyMode::kSouthTeamId));
}

TEST(SupremacyMode, AddsConnectedValueDifferenceEveryFiveSeconds) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);
    ASSERT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    // South = Command Post (1) + Hospital (2), North = Temple (1).
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kSouthTeamId), 3);
    EXPECT_EQ(mode.GetTeamObjectiveValue(SupremacyMode::kNorthTeamId), 1);

    mode.Update(4.0f);
    EXPECT_EQ(mode.GetScore(), 0);
    mode.Update(1.0f);
    EXPECT_EQ(mode.GetScore(), 2);

    // Catch-up updates apply every elapsed interval, not just one.
    mode.Update(10.0f);
    EXPECT_EQ(mode.GetScore(), 6);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kSouthTeamId), 253.0f);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kNorthTeamId), 247.0f);
    EXPECT_NEAR(mode.GetPointBarProgress(), 0.506f, 0.0001f);
}

TEST(SupremacyMode, SignedScoreReversesWhenConnectedControlChanges) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    mode.OnObjectiveCaptured(3, SupremacyMode::kNorthTeamId);
    mode.OnObjectiveCaptured(4, SupremacyMode::kNorthTeamId);
    // South = 1, North = Temple (1) + Ammo (1) + Artillery (3).
    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), -4);

    mode.OnObjectiveCaptured(2, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(3, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(4, SupremacyMode::kSouthTeamId);
    // South = 1 + 2 + 1 + 3, North = 1: a +6 tick crosses through zero.
    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), 2);
}

TEST(SupremacyMode, ClampsAtConfigurableSignedTarget) {
    SupremacyMode mode(nullptr);
    mode.SetScoreTarget(7);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(2, SupremacyMode::kSouthTeamId);
    mode.OnObjectiveCaptured(4, SupremacyMode::kSouthTeamId);

    mode.Update(15.0f);
    EXPECT_EQ(mode.GetScore(), 7);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::PostRound);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kSouthTeamId), 7.0f);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kNorthTeamId), 0.0f);
}

TEST(SupremacyMode, MaximumAcceptedScoreTargetUsesOverflowSafeViews) {
    SupremacyMode mode(nullptr);
    mode.SetScoreTarget(std::numeric_limits<int32_t>::max());
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    EXPECT_TRUE(std::isfinite(mode.GetTeamPoints(SupremacyMode::kSouthTeamId)));
    EXPECT_TRUE(std::isfinite(mode.GetTeamPoints(SupremacyMode::kNorthTeamId)));
    EXPECT_FLOAT_EQ(mode.GetPointBarProgress(), 0.5f);

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.Update(5.0f);
    EXPECT_EQ(mode.GetScore(), 2);
    EXPECT_TRUE(std::isfinite(mode.GetPointBarProgress()));
}

TEST(SupremacyMode, CatchUpFromOppositeBoundAvoidsNarrowingAndOverflow) {
    SupremacyMode mode(nullptr);
    mode.SetScoreTarget(std::numeric_limits<int32_t>::max());
    mode.SetScoringInterval(1.0f);
    mode.SetRoundTime(10.0f);
    mode.ClearObjectives();

    constexpr int kNearlyMaximumValue =
        std::numeric_limits<int32_t>::max() - 1;
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 0);
    mode.SetObjectiveMetadata(
        1, SupremacyMode::kNorthTeamId, kNearlyMaximumValue);
    mode.SetObjectiveLinks({{0, {1}}, {1, {0}}});
    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 1);
    BeginActiveRound(mode);

    mode.Update(1.0f);
    ASSERT_EQ(mode.GetScore(), -kNearlyMaximumValue);

    // Two positive ticks total 2*(INT32_MAX-1), which is not representable in
    // int32 but stops one point short of the positive target from this score.
    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.Update(2.0f);
    EXPECT_EQ(mode.GetScore(), kNearlyMaximumValue);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);
}

TEST(SupremacyMode, ActivePhaseExpiryScoresOnlyTimeBeforeDeadline) {
    SupremacyMode oneUpdate(nullptr);
    SupremacyMode splitAtDeadline(nullptr);
    for (SupremacyMode* mode : {&oneUpdate, &splitAtDeadline}) {
        mode->SetRoundTime(6.0f);
        ConfigureHueCity(*mode);
        BeginActiveRound(*mode);
        mode->OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    }

    oneUpdate.Update(10.0f);
    splitAtDeadline.Update(6.0f);
    splitAtDeadline.Update(4.0f);

    EXPECT_EQ(oneUpdate.GetScore(), 2);
    EXPECT_EQ(oneUpdate.GetScore(), splitAtDeadline.GetScore());
    EXPECT_EQ(oneUpdate.GetWinningTeam(), splitAtDeadline.GetWinningTeam());
    EXPECT_EQ(oneUpdate.GetPhase(), SupremacyMode::Phase::PostRound);
    EXPECT_EQ(splitAtDeadline.GetPhase(), SupremacyMode::Phase::PostRound);
}

TEST(SupremacyMode, SouthEliminationAwardsNorthDespiteSouthScoreLead) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.Update(5.0f);
    ASSERT_GT(mode.GetScore(), 0);

    mode.OnTicketsDepleted(SupremacyMode::kSouthTeamId);
    ASSERT_EQ(mode.GetPhase(), SupremacyMode::Phase::SuddenDeath);
    mode.OnTeamEliminated(SupremacyMode::kSouthTeamId);

    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::PostRound);
    EXPECT_EQ(mode.GetWinningTeam(), SupremacyMode::kNorthTeamId);
    EXPECT_GT(mode.GetScore(), 0);
}

TEST(SupremacyMode, LegacyStartingPointsMapsToSignedTargetAndRoundResets) {
    SupremacyMode mode(nullptr);
    mode.SetStartingPoints(300.0f);
    mode.SetRoundTime(900.0f);
    ConfigureHueCity(mode);
    BeginActiveRound(mode);

    EXPECT_EQ(mode.GetScoreTarget(), 600);
    EXPECT_FLOAT_EQ(mode.GetRoundDuration(), 900.0f);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), 900.0f);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kSouthTeamId), 300.0f);
    EXPECT_FLOAT_EQ(mode.GetTeamPoints(SupremacyMode::kNorthTeamId), 300.0f);

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.Update(5.0f);
    ASSERT_NE(mode.GetScore(), 0);

    mode.StartRound();
    EXPECT_EQ(mode.GetScore(), 0);
    EXPECT_EQ(mode.GetObjectiveControllingTeam(1), 0u);
}

TEST(SupremacyMode, RejectsMalformedTimeAndWrongPhaseAuthorityEvents) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    mode.Initialize();

    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.OnTicketsDepleted(SupremacyMode::kSouthTeamId);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::WarmUp);
    EXPECT_EQ(mode.GetObjectiveControllingTeam(1), 0u);

    BeginActiveRound(mode);
    const float activeTime = mode.GetPhaseTimeRemaining();
    mode.Update(-1.0f);
    mode.Update(std::numeric_limits<float>::quiet_NaN());
    mode.Update(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), activeTime);
    EXPECT_EQ(mode.GetScore(), 0);

    mode.OnObjectiveCaptured(1, 0);
    mode.OnObjectiveCaptured(999, SupremacyMode::kSouthTeamId);
    mode.OnTicketsDepleted(99);
    EXPECT_EQ(mode.GetObjectiveControllingTeam(1), 0u);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::Active);

    mode.OnTicketsDepleted(SupremacyMode::kSouthTeamId);
    ASSERT_EQ(mode.GetPhase(), SupremacyMode::Phase::SuddenDeath);
    mode.OnTicketsDepleted(SupremacyMode::kNorthTeamId);
    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::SuddenDeath);
    EXPECT_EQ(mode.GetObjectiveControllingTeam(1), 0u);
}

TEST(SupremacyMode, ReinitializeRestoresFirstRoundStateAndNullWarmupIsSafe) {
    SupremacyMode mode(nullptr);
    ConfigureHueCity(mode);
    mode.Initialize();
    BeginActiveRound(mode);
    mode.OnObjectiveCaptured(1, SupremacyMode::kSouthTeamId);
    mode.Update(5.0f);
    ASSERT_NE(mode.GetScore(), 0);

    mode.Initialize();
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::WarmUp);
    EXPECT_EQ(mode.GetScore(), 0);
    EXPECT_EQ(mode.GetWinningTeam(), 0u);
    EXPECT_EQ(mode.GetObjectiveControllingTeam(1), 0u);
    EXPECT_NO_THROW(mode.Update(0.0f));
    EXPECT_EQ(mode.GetPhase(), SupremacyMode::Phase::WarmUp);
}

RS2V_TEST_MAIN()
