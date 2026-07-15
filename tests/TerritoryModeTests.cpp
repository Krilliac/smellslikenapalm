#include "TestFramework.h"

#include "Game/GameServer.h"
#include "Game/ObjectiveSystem.h"
#include "Game/TeamMapping.h"
#include "Game/TerritoryMode.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct TerritoryModeTestAccess {
    static bool AnyAttackerInCurrentPhase(
        const ObjectiveSystem& objectives,
        uint32_t attackingTeam,
        const std::function<bool(uint32_t)>& isAttackingPlayer) {
        return TerritoryMode::AnyAttackerInCurrentTerritoryPhase(
            objectives, attackingTeam, isAttackingPlayer);
    }

    static void SetRoundResult(TerritoryMode& mode,
                               int round,
                               int objectivesCaptured,
                               uint32_t attackingTeam,
                               uint32_t usTickets,
                               uint32_t nvaTickets) {
        mode.m_objectivesCapturedRound[round] = objectivesCaptured;
        mode.m_attackingTeamRound[round] = attackingTeam;
        mode.m_ticketsRemainingRound[round][0] = usTickets;
        mode.m_ticketsRemainingRound[round][1] = nvaTickets;
    }

    static void DetermineWinner(TerritoryMode& mode) {
        mode.DetermineWinner();
    }

    static void ForcePhase(TerritoryMode& mode, TerritoryMode::Phase phase) {
        mode.SetPhase(phase);
    }

    static void AdvanceActive(TerritoryMode& mode,
                              float deltaSeconds,
                              bool attackersContesting,
                              bool lockdownObjectiveEligible) {
        mode.AdvanceActivePhase(deltaSeconds, attackersContesting,
                                lockdownObjectiveEligible);
    }

    static void ResetLockdownAtCurrentTime(TerritoryMode& mode,
                                           bool eligible) {
        mode.SynchronizeLockdownEligibility(
            eligible, mode.m_phaseTimer, true);
    }

    static bool CurrentPhaseSupportsLockdown(
        const ObjectiveSystem& objectives,
        uint32_t defendingTeam) {
        return TerritoryMode::CurrentTerritoryPhaseSupportsLockdown(
            objectives, defendingTeam);
    }
};

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

CaptureZone MakeTerritoryZone(uint32_t id, const char* name, int phase) {
    CaptureZone zone;
    zone.id = id;
    zone.name = name;
    zone.type = ObjectiveType::Territory;
    zone.territoryOrder = phase;
    return zone;
}

CaptureZone MakeLockdownTerritoryZone(uint32_t id,
                                      const char* name,
                                      int phase,
                                      uint32_t defendingTeam) {
    CaptureZone zone = MakeTerritoryZone(id, name, phase);
    zone.controllingTeam = defendingTeam;
    zone.lockdownMetadataKnown = true;
    zone.lockdownEnabled = true;
    zone.lockdownTimeSecondsByPlayerBand = {{240, 240, 240}};
    return zone;
}

} // namespace

TEST(TerritoryMode, FinishedTransitionBroadcastsMatchCompletionExactlyOnce) {
    RecordingGameServer server;
    TerritoryMode mode(&server);
    mode.Initialize();

    mode.StartRound();
    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::PostRound);
    mode.EndRound();
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::HalfTime);
    mode.SwitchSides();
    ASSERT_EQ(mode.GetCurrentRound(), 1);

    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::PostRound);
    mode.EndRound();

    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::Finished);
    EXPECT_EQ(server.CountChatMessage("[Territory] Match complete!"), 1u);
}

TEST(TerritoryMode, SuddenDeathIsEliminationOnlyAndRejectsCapture) {
    GameServer server;
    TerritoryMode mode(&server);
    mode.Initialize();
    mode.SetPreparationTime(1.0f);

    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::Preparation);
    EXPECT_FALSE(mode.CanCaptureObjectives());

    mode.Update(1.0f);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::Active);
    EXPECT_TRUE(mode.CanCaptureObjectives());

    mode.OnTicketsDepleted(TeamMapping::kServerUs);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::SuddenDeath);
    EXPECT_FALSE(mode.CanCaptureObjectives());
}

TEST(TerritoryMode, ContestingChecksEveryActiveSiblingButNotFuturePhases) {
    ObjectiveSystem objectives(nullptr);
    objectives.Initialize();
    objectives.AddObjective(MakeTerritoryZone(1, "Villa", 0));
    objectives.AddObjective(MakeTerritoryZone(2, "Farm", 0));
    objectives.AddObjective(MakeTerritoryZone(3, "Temple", 1));
    objectives.SetTerritoryOrder({1, 2, 3});

    CaptureZone* villa = objectives.GetObjective(1);
    CaptureZone* farm = objectives.GetObjective(2);
    CaptureZone* temple = objectives.GetObjective(3);
    ASSERT_NE(villa, nullptr);
    ASSERT_NE(farm, nullptr);
    ASSERT_NE(temple, nullptr);

    const auto isAttacker = [](uint32_t playerId) { return playerId == 42; };

    // Simulate one member of a grouped phase already captured/locked. Presence
    // on the still-active sibling must continue the phase's overtime.
    villa->isActive = false;
    farm->defenderIds.push_back(42);
    EXPECT_TRUE(TerritoryModeTestAccess::AnyAttackerInCurrentPhase(
        objectives, TeamMapping::kServerUs, isAttacker));

    farm->defenderIds.clear();
    EXPECT_FALSE(TerritoryModeTestAccess::AnyAttackerInCurrentPhase(
        objectives, TeamMapping::kServerUs, isAttacker));

    // Headless occupancy prolongs overtime just like a human in the same
    // active phase. No synthetic player id is needed.
    farm->botCaptureWeightByTeam[TeamMapping::kServerUs] = 1.0f;
    EXPECT_TRUE(TerritoryModeTestAccess::AnyAttackerInCurrentPhase(
        objectives, TeamMapping::kServerUs, isAttacker));
    farm->botCaptureWeightByTeam[TeamMapping::kServerUs] = 0.0f;

    // Even malformed state that marks a future objective active cannot extend
    // overtime for the current grouped phase.
    temple->isActive = true;
    temple->attackerIds.push_back(42);
    temple->botCaptureWeightByTeam[TeamMapping::kServerUs] = 1.0f;
    EXPECT_FALSE(TerritoryModeTestAccess::AnyAttackerInCurrentPhase(
        objectives, TeamMapping::kServerUs, isAttacker));
}

TEST(TerritoryMode, TicketSnapshotsAndTiebreakRemainUnsigned) {
    static_assert(std::is_same_v<
        decltype(std::declval<const TerritoryMode&>().GetRoundTicketsRemaining(
            0, TeamMapping::kServerUs)),
        uint32_t>);

    GameServer server;
    TerritoryMode mode(&server);
    mode.Initialize();

    TerritoryModeTestAccess::SetRoundResult(
        mode, 0, 2, TeamMapping::kServerUs,
        std::numeric_limits<uint32_t>::max(), 0u);
    TerritoryModeTestAccess::SetRoundResult(
        mode, 1, 2, TeamMapping::kServerNva, 0u, 1u);

    EXPECT_EQ(mode.GetRoundTicketsRemaining(0, TeamMapping::kServerUs),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(mode.GetRoundTicketsRemaining(1, TeamMapping::kServerNva), 1u);
    EXPECT_EQ(mode.GetRoundTicketsRemaining(0, 99u), 0u);

    // Narrowing UINT32_MAX to int would turn it negative and incorrectly award
    // this tied match to the round-two attacker.
    TerritoryModeTestAccess::DetermineWinner(mode);
    EXPECT_EQ(mode.GetWinningTeam(), TeamMapping::kServerUs);
}

TEST(TerritoryMode, RejectsMalformedTimeAndWrongPhaseEvents) {
    TerritoryMode mode(nullptr);
    mode.Initialize();
    mode.SetRoundTime(42.0f);
    mode.SetRoundTime(-1.0f);
    mode.SetRoundTime(std::numeric_limits<float>::quiet_NaN());
    mode.SetRoundTime(std::numeric_limits<float>::infinity());
    mode.SetPreparationTime(1.0f);

    mode.StartRound();
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::Preparation);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 1.0f);
    mode.OnObjectiveCaptured(1, TeamMapping::kServerUs);
    mode.OnTicketsDepleted(TeamMapping::kServerUs);
    mode.Update(-1.0f);
    mode.Update(std::numeric_limits<float>::quiet_NaN());
    mode.Update(std::numeric_limits<float>::infinity());
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::Preparation);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), 1.0f);
    EXPECT_EQ(mode.GetAttackerObjectivesCaptured(0), 0);

    mode.Update(1.0f);
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::Active);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 42.0f);
    mode.OnTicketsDepleted(99);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::Active);
    mode.OnTicketsDepleted(TeamMapping::kServerUs);
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::SuddenDeath);
    mode.OnTicketsDepleted(TeamMapping::kServerNva);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::SuddenDeath);
    EXPECT_EQ(mode.GetAttackerObjectivesCaptured(0), 0);
}

TEST(TerritoryMode, MissingObjectivesFailClosedAtDeadlineWithoutCrashing) {
    GameServer server;
    TerritoryMode mode(&server);
    mode.Initialize();
    mode.SetPreparationTime(0.0f);
    mode.SetRoundTime(0.0f);
    mode.SetPostRoundTime(0.0f);

    mode.StartRound();
    mode.Update(0.0f);
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::Active);
    EXPECT_NO_THROW(mode.Update(0.0f));
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::PostRound);

    mode.Update(0.0f);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::HalfTime);
    mode.EndRound();
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::HalfTime);
}

TEST(TerritoryMode, HalftimeSwapIsOneShotAndReinitializeRestoresSides) {
    TerritoryMode mode(nullptr);
    mode.Initialize();
    EXPECT_EQ(mode.GetAttackingTeam(), TeamMapping::kServerUs);
    EXPECT_EQ(mode.GetDefendingTeam(), TeamMapping::kServerNva);

    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::PostRound);
    mode.EndRound();
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::HalfTime);
    mode.SwitchSides();
    ASSERT_EQ(mode.GetPhase(), TerritoryMode::Phase::Preparation);
    EXPECT_EQ(mode.GetCurrentRound(), 1);
    EXPECT_EQ(mode.GetAttackingTeam(), TeamMapping::kServerNva);
    EXPECT_EQ(mode.GetDefendingTeam(), TeamMapping::kServerUs);

    mode.SwitchSides();
    EXPECT_EQ(mode.GetCurrentRound(), 1);
    EXPECT_EQ(mode.GetAttackingTeam(), TeamMapping::kServerNva);

    mode.Initialize();
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::WarmUp);
    EXPECT_EQ(mode.GetCurrentRound(), 0);
    EXPECT_EQ(mode.GetAttackingTeam(), TeamMapping::kServerUs);
    EXPECT_EQ(mode.GetDefendingTeam(), TeamMapping::kServerNva);
    EXPECT_EQ(mode.GetWinningTeam(), 0u);
}

TEST(TerritoryMode, RetailTimingDefaultsRejectMalformedConfiguration) {
    TerritoryMode mode(nullptr);
    mode.Initialize();

    EXPECT_FLOAT_EQ(mode.GetRoundDuration(), 600.0f);
    EXPECT_FLOAT_EQ(mode.GetLockdownDuration(), 300.0f);
    EXPECT_FLOAT_EQ(mode.GetCaptureLockdownDelay(), 600.0f);
    EXPECT_FLOAT_EQ(mode.GetCaptureAttemptLockdownDelay(), 420.0f);
    EXPECT_FLOAT_EQ(mode.GetOvertimeDuration(), 180.0f);
    EXPECT_EQ(mode.GetRetailTimingState().nextLockdownTime, -1);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, -1);
    EXPECT_FALSE(mode.GetRetailTimingState().overtime);

    mode.SetLockdownTime(-1.0f);
    mode.SetLockdownTime(std::numeric_limits<float>::quiet_NaN());
    mode.SetCaptureLockdownDelay(-1.0f);
    mode.SetCaptureLockdownDelay(std::numeric_limits<float>::infinity());
    mode.SetCaptureAttemptLockdownDelay(-1.0f);
    mode.SetCaptureAttemptLockdownDelay(
        std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(mode.GetLockdownDuration(), 300.0f);
    EXPECT_FLOAT_EQ(mode.GetCaptureLockdownDelay(), 600.0f);
    EXPECT_FLOAT_EQ(mode.GetCaptureAttemptLockdownDelay(), 420.0f);

    mode.SetRoundTime(1200.0f);
    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::Active);
    TerritoryModeTestAccess::AdvanceActive(mode, 0.0f, false, true);
    const auto before = mode.GetRetailTimingState();
    const float remainingBefore = mode.GetRoundTimeRemaining();
    TerritoryModeTestAccess::AdvanceActive(
        mode, std::numeric_limits<float>::quiet_NaN(), true, false);
    TerritoryModeTestAccess::AdvanceActive(mode, -1.0f, true, false);
    TerritoryModeTestAccess::AdvanceActive(
        mode, std::numeric_limits<float>::infinity(), true, false);
    const auto after = mode.GetRetailTimingState();
    EXPECT_EQ(after.nextLockdownTime, before.nextLockdownTime);
    EXPECT_EQ(after.nextEstimatedLockdownTime,
              before.nextEstimatedLockdownTime);
    EXPECT_EQ(after.overtime, before.overtime);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), remainingBefore);
}

TEST(TerritoryMode, MainDeadlineNeverAddsLockdownPhase) {
    TerritoryMode uncontested(nullptr);
    uncontested.Initialize();
    uncontested.SetRoundTime(10.0f);
    TerritoryModeTestAccess::ForcePhase(
        uncontested, TerritoryMode::Phase::Active);
    TerritoryModeTestAccess::AdvanceActive(uncontested, 10.0f, false, false);
    EXPECT_EQ(uncontested.GetPhase(), TerritoryMode::Phase::PostRound);
    EXPECT_FALSE(uncontested.GetPhase() == TerritoryMode::Phase::Lockdown);

    TerritoryMode contested(nullptr);
    contested.Initialize();
    contested.SetRoundTime(10.0f);
    TerritoryModeTestAccess::ForcePhase(
        contested, TerritoryMode::Phase::Active);
    TerritoryModeTestAccess::AdvanceActive(contested, 10.0f, true, false);
    EXPECT_EQ(contested.GetPhase(), TerritoryMode::Phase::Overtime);
    EXPECT_FLOAT_EQ(contested.GetRoundTimeRemaining(), 180.0f);
    EXPECT_TRUE(contested.GetRetailTimingState().overtime);
    EXPECT_EQ(contested.GetRetailTimingState().nextLockdownTime, -1);
}

TEST(TerritoryMode, EarlyLockdownUsesAbsoluteRoundClockTarget) {
    TerritoryMode defendersHold(nullptr);
    defendersHold.Initialize();
    defendersHold.SetRoundTime(1200.0f);
    TerritoryModeTestAccess::ForcePhase(
        defendersHold, TerritoryMode::Phase::Active);

    TerritoryModeTestAccess::AdvanceActive(
        defendersHold, 0.0f, false, true);
    EXPECT_EQ(defendersHold.GetRetailTimingState().nextLockdownTime, -1);
    EXPECT_EQ(
        defendersHold.GetRetailTimingState().nextEstimatedLockdownTime, 480);

    TerritoryModeTestAccess::AdvanceActive(
        defendersHold, 420.0f, false, true);
    EXPECT_EQ(defendersHold.GetRetailTimingState().nextLockdownTime, 480);
    EXPECT_EQ(
        defendersHold.GetRetailTimingState().nextEstimatedLockdownTime, -1);
    EXPECT_EQ(defendersHold.GetPhase(), TerritoryMode::Phase::Active);

    TerritoryModeTestAccess::AdvanceActive(
        defendersHold, 299.5f, false, true);
    EXPECT_EQ(defendersHold.GetPhase(), TerritoryMode::Phase::Active);
    EXPECT_FLOAT_EQ(defendersHold.GetRoundTimeRemaining(), 480.5f);
    TerritoryModeTestAccess::AdvanceActive(
        defendersHold, 0.5f, false, true);
    EXPECT_EQ(defendersHold.GetPhase(), TerritoryMode::Phase::PostRound);

    TerritoryMode contested(nullptr);
    contested.Initialize();
    contested.SetRoundTime(1200.0f);
    TerritoryModeTestAccess::ForcePhase(
        contested, TerritoryMode::Phase::Active);
    TerritoryModeTestAccess::AdvanceActive(contested, 420.0f, false, true);
    ASSERT_EQ(contested.GetRetailTimingState().nextLockdownTime, 480);
    TerritoryModeTestAccess::AdvanceActive(contested, 300.0f, true, true);
    EXPECT_EQ(contested.GetPhase(), TerritoryMode::Phase::Overtime);
    EXPECT_FLOAT_EQ(contested.GetRoundTimeRemaining(), 180.0f);
}

TEST(TerritoryMode, NewCaptureAttemptDefersOnlyUnarmedSchedule) {
    TerritoryMode mode(nullptr);
    mode.Initialize();
    mode.SetRoundTime(1200.0f);
    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::Active);
    TerritoryModeTestAccess::AdvanceActive(mode, 0.0f, false, true);
    ASSERT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, 480);

    TerritoryModeTestAccess::AdvanceActive(mode, 100.0f, false, true);
    TerritoryModeTestAccess::AdvanceActive(mode, 1.0f, true, true);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, 379);

    // Continuous capture pressure is the same attempt and must not slide.
    TerritoryModeTestAccess::AdvanceActive(mode, 100.0f, true, true);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, 379);

    // A completed capture resets both inactivity baselines at 999 seconds.
    TerritoryModeTestAccess::ResetLockdownAtCurrentTime(mode, true);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, 279);
    TerritoryModeTestAccess::ResetLockdownAtCurrentTime(mode, false);
    EXPECT_EQ(mode.GetRetailTimingState().nextLockdownTime, -1);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, -1);
}

TEST(TerritoryMode, CoarseDeltaCannotSkipEarlyLockdownBoundary) {
    TerritoryMode mode(nullptr);
    mode.Initialize();
    mode.SetRoundTime(1200.0f);
    TerritoryModeTestAccess::ForcePhase(mode, TerritoryMode::Phase::Active);

    // This single frame crosses the 780-second inactivity trigger and the
    // 480-second lockdown target but not the ordinary zero deadline.
    TerritoryModeTestAccess::AdvanceActive(mode, 1000.0f, false, true);
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::PostRound);

    mode.Initialize();
    EXPECT_EQ(mode.GetPhase(), TerritoryMode::Phase::WarmUp);
    EXPECT_EQ(mode.GetRetailTimingState().nextLockdownTime, -1);
    EXPECT_EQ(mode.GetRetailTimingState().nextEstimatedLockdownTime, -1);
}

TEST(TerritoryMode, LockdownEligibilityFailsClosedPerCurrentPhase) {
    ObjectiveSystem objectives(nullptr);
    objectives.Initialize();
    objectives.AddObjective(MakeLockdownTerritoryZone(
        1, "Villa", 0, TeamMapping::kServerNva));
    objectives.AddObjective(MakeLockdownTerritoryZone(
        2, "Farm", 0, TeamMapping::kServerNva));

    // Future malformed metadata is irrelevant even if runtime state marks the
    // future zone active; only the current Territory phase participates.
    CaptureZone future = MakeTerritoryZone(3, "Temple", 1);
    future.controllingTeam = TeamMapping::kServerNva;
    future.lockdownTimeSecondsByPlayerBand = {{-1, 0, 0}};
    objectives.AddObjective(future);
    objectives.SetTerritoryOrder({1, 2, 3});
    ASSERT_NE(objectives.GetObjective(3), nullptr);
    objectives.GetObjective(3)->isActive = true;

    EXPECT_TRUE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, TeamMapping::kServerNva));

    CaptureZone* villa = objectives.GetObjective(1);
    CaptureZone* farm = objectives.GetObjective(2);
    ASSERT_NE(villa, nullptr);
    ASSERT_NE(farm, nullptr);

    villa->lockdownEnabled = false;
    farm->lockdownEnabled = false;
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, TeamMapping::kServerNva));

    villa->lockdownEnabled = true;
    farm->lockdownMetadataKnown = false;
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, TeamMapping::kServerNva));

    farm->lockdownMetadataKnown = true;
    farm->lockdownTimeSecondsByPlayerBand[1] = -1;
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, TeamMapping::kServerNva));

    farm->lockdownTimeSecondsByPlayerBand[1] = 240;
    villa->controllingTeam = TeamMapping::kServerUs;
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, TeamMapping::kServerNva));
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        objectives, 99u));

    ObjectiveSystem empty(nullptr);
    empty.Initialize();
    EXPECT_FALSE(TerritoryModeTestAccess::CurrentPhaseSupportsLockdown(
        empty, TeamMapping::kServerNva));
}

RS2V_TEST_MAIN()
