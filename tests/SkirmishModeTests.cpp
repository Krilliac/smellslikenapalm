#include "Game/GameServer.h"
#include "Game/SkirmishMode.h"
#include "Game/SpawnSystem.h"
#include "TestFramework.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

void AdvanceToActive(SkirmishMode& mode) {
    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
    mode.Update(15.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
}

} // namespace

TEST(SkirmishMode, FinishedTransitionBroadcastsMatchCompletionExactlyOnce) {
    RecordingGameServer server;
    SkirmishMode mode(&server);
    mode.Initialize();

    for (int round = 0; round < 3; ++round) {
        if (round == 0) {
            AdvanceToActive(mode);
        } else {
            ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
            mode.Update(15.0f);
            ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
        }
        mode.OnTicketsDepleted(SkirmishMode::kSouthTeamId);
        ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::PostRound);
        mode.Update(10.0f);

        if (round < 2) {
            ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::NextRound);
            mode.Update(5.0f);
        }
    }

    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Finished);
    EXPECT_EQ(server.CountChatMessage("[Skirmish] Match complete!"), 1u);
}

TEST(SkirmishMode, PreparationDoesNotPermitObjectiveCapture) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    EXPECT_FALSE(mode.CanCaptureObjectives());

    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
    EXPECT_FALSE(mode.CanCaptureObjectives());

    mode.Update(15.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
    EXPECT_TRUE(mode.CanCaptureObjectives());
}

TEST(SkirmishMode, PhaseClockCoversPreparationSuddenDeathAndRoundBreaks) {
    SkirmishMode mode(nullptr);
    mode.SetRoundTime(1.0f);
    mode.Initialize();
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 0.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 0.0f);

    mode.StartRound();
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 15.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 15.0f);

    mode.Update(15.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 1.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 1.0f);

    mode.Update(1.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::InstantDeath);
    EXPECT_TRUE(mode.CanCaptureObjectives());
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 120.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 120.0f);

    mode.OnTicketsDepleted(SkirmishMode::kSouthTeamId);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::PostRound);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 10.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 10.0f);

    mode.Update(10.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::NextRound);
    EXPECT_FLOAT_EQ(mode.GetPhaseDuration(), 5.0f);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 5.0f);
}

TEST(SkirmishMode, RetailDefaultsExposeBoundedWireState) {
    SkirmishMode mode(nullptr);
    mode.Initialize();

    const std::array<int32_t, 20> windows = mode.GetAllSpawnWindows();
    EXPECT_EQ(windows[0], 100);
    EXPECT_EQ(windows[1], 75);
    EXPECT_EQ(windows[2], 50);
    EXPECT_EQ(windows[3], 25);
    EXPECT_EQ(windows[4], 0);
    for (std::size_t i = 5; i < windows.size(); ++i) {
        EXPECT_EQ(windows[i], -1) << "h37 unused slot " << i;
    }

    const SkirmishMode::RetailState state = mode.GetRetailState();
    EXPECT_EQ(state.allSpawnWindows, windows);
    EXPECT_EQ(state.spawnWindowCloseTime[0], 0);
    EXPECT_EQ(state.spawnWindowCloseTime[1], 0);
    EXPECT_EQ(state.playedRoundsCount, 0);
    EXPECT_EQ(state.roundTeamScoreLimit, 10);
    EXPECT_EQ(state.roundLimit, 5);
    EXPECT_EQ(state.nextLockDownTime, -1);
    EXPECT_FALSE(state.suddenDeath);
    EXPECT_FALSE(state.overTime);
    EXPECT_EQ(state.teamWithOvertimeAdvantage, TeamMapping::kRetailNeutral);
    EXPECT_EQ(state.playersAliveCount[TeamMapping::kRetailNva], 0);
    EXPECT_EQ(state.playersAliveCount[TeamMapping::kRetailUs], 0);
    EXPECT_FLOAT_EQ(mode.GetRoundDuration(), 300.0f);
}

TEST(SkirmishMode, ActiveRoundUsesFiveTwentyFiveSecondWaves) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    const auto closeTimes = mode.GetSpawnWindowCloseTimes();
    EXPECT_EQ(closeTimes[TeamMapping::kRetailNva], 200);
    EXPECT_EQ(closeTimes[TeamMapping::kRetailUs], 200);
    EXPECT_TRUE(mode.IsSpawnWindowOpen(SkirmishMode::kNorthTeamId));
    EXPECT_TRUE(mode.IsSpawnWindowOpen(SkirmishMode::kSouthTeamId));
    EXPECT_EQ(mode.GetSpawnWavesRemaining(SkirmishMode::kSouthTeamId), 5);
    EXPECT_EQ(mode.GetNextSpawnWaveTime(SkirmishMode::kSouthTeamId), 0);

    mode.Update(1.0f);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), 299.0f);
    EXPECT_EQ(mode.GetSpawnWavesRemaining(SkirmishMode::kSouthTeamId), 4);
    EXPECT_EQ(mode.GetNextSpawnWaveTime(SkirmishMode::kSouthTeamId), 24);

    mode.Update(24.0f);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), 275.0f);
    EXPECT_EQ(mode.GetSpawnWavesRemaining(SkirmishMode::kSouthTeamId), 4);
    EXPECT_EQ(mode.GetNextSpawnWaveTime(SkirmishMode::kSouthTeamId), 0);
}

TEST(SkirmishMode, PreparationAndSuddenDeathCloseAuthoritativeRespawns) {
    SkirmishMode mode(nullptr);
    mode.SetRoundTime(1.0f);
    mode.Initialize();

    mode.StartRound();
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kSouthTeamId));
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kNorthTeamId));

    mode.Update(15.0f);
    EXPECT_TRUE(mode.IsSpawnWindowOpen(SkirmishMode::kSouthTeamId));
    EXPECT_TRUE(mode.IsSpawnWindowOpen(SkirmishMode::kNorthTeamId));

    mode.Update(1.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::InstantDeath);
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kSouthTeamId));
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kNorthTeamId));
}

TEST(SkirmishMode, FinalWaveAllowsCloseBoundaryButRejectsLoadedTickPastIt) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    // One second before h38 close, a normal one-second step lands exactly on
    // the final h37 opportunity. A loaded two-second step has already missed it.
    mode.Update(99.0f);
    EXPECT_FLOAT_EQ(mode.GetRoundTimeRemaining(), 201.0f);
    EXPECT_TRUE(mode.CanReleaseSpawnWave(SkirmishMode::kSouthTeamId, 1.0f));
    EXPECT_FALSE(mode.CanReleaseSpawnWave(SkirmishMode::kSouthTeamId, 2.0f));
}

TEST(SkirmishMode, SpawnSystemUsesConfiguredRetailWaveCadenceForNewTeams) {
    SpawnSystem spawns(nullptr);
    spawns.Initialize();
    spawns.SetWaveInterval(
        static_cast<float>(SkirmishMode::kSpawnWaveIntervalSeconds));

    spawns.StartSpawnWave(SkirmishMode::kSouthTeamId);
    EXPECT_TRUE(spawns.IsInSpawnWave(SkirmishMode::kSouthTeamId));
    EXPECT_FLOAT_EQ(
        spawns.GetWaveTimeRemaining(SkirmishMode::kSouthTeamId), 25.0f);

    spawns.SetWaveTimeRemaining(SkirmishMode::kSouthTeamId, 19.0f);
    EXPECT_FLOAT_EQ(
        spawns.GetWaveTimeRemaining(SkirmishMode::kSouthTeamId), 19.0f);
}

TEST(SkirmishMode, ObjectiveCaptureExtendsOnlyCapturingTeamsWindow) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);
    mode.Update(80.0f);

    mode.OnObjectiveCaptured(1, SkirmishMode::kSouthTeamId);

    const auto closeTimes = mode.GetSpawnWindowCloseTimes();
    EXPECT_EQ(closeTimes[TeamMapping::kRetailNva], 200);
    EXPECT_EQ(closeTimes[TeamMapping::kRetailUs], 176);
    EXPECT_EQ(mode.GetNextSpawnWaveTime(SkirmishMode::kSouthTeamId), 19);
    EXPECT_EQ(mode.GetNextSpawnWaveTime(SkirmishMode::kNorthTeamId), 20);

    mode.OnObjectiveCaptured(1, SkirmishMode::kSouthTeamId);
    EXPECT_EQ(mode.GetSpawnWindowCloseTimes()[TeamMapping::kRetailUs], 176);
    mode.OnObjectiveCaptured(1, SkirmishMode::kNorthTeamId);
    EXPECT_EQ(mode.GetSpawnWindowCloseTimes()[TeamMapping::kRetailNva], 176);
}

TEST(SkirmishMode, AllObjectiveLockdownReplicatesAndAwardsAfterSixtySeconds) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    mode.OnObjectiveControlChanged(SkirmishMode::kSouthTeamId, true, false);
    SkirmishMode::RetailState state = mode.GetRetailState();
    EXPECT_TRUE(state.overTime);
    EXPECT_FALSE(state.suddenDeath);
    EXPECT_EQ(state.nextLockDownTime, 240);
    EXPECT_EQ(state.teamWithOvertimeAdvantage, TeamMapping::kRetailUs);
    EXPECT_EQ(mode.GetOvertimeAdvantageTeam(), SkirmishMode::kSouthTeamId);
    EXPECT_FLOAT_EQ(mode.GetLockdownTimeRemaining(), 60.0f);

    mode.Update(59.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
    EXPECT_EQ(mode.GetTeamRoundWins(SkirmishMode::kSouthTeamId), 0);

    mode.Update(1.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::PostRound);
    EXPECT_EQ(mode.GetTeamRoundWins(SkirmishMode::kSouthTeamId), 1);

    mode.Update(10.0f);
    EXPECT_EQ(mode.GetPlayedRoundsCount(), 1);
    EXPECT_EQ(mode.GetRetailState().playedRoundsCount, 1);
}

TEST(SkirmishMode, ContestedObjectiveDefersExpiredLockdownUntilClear) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    mode.OnObjectiveControlChanged(SkirmishMode::kNorthTeamId, true, true);
    mode.Update(60.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
    EXPECT_TRUE(mode.IsInOvertime());
    EXPECT_FLOAT_EQ(mode.GetLockdownTimeRemaining(), 0.0f);

    // A repeated authoritative snapshot clears contesting without moving the
    // original target. The next zero-delta state tick resolves the lockdown.
    mode.OnObjectiveControlChanged(SkirmishMode::kNorthTeamId, true, false);
    EXPECT_EQ(mode.GetNextLockdownTime(), 240);
    mode.Update(0.0f);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::PostRound);
    EXPECT_EQ(mode.GetTeamRoundWins(SkirmishMode::kNorthTeamId), 1);
}

TEST(SkirmishMode, LostAllObjectiveControlCancelsOvertimeState) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    mode.OnObjectiveControlChanged(SkirmishMode::kSouthTeamId, true, false);
    mode.OnObjectiveControlChanged(SkirmishMode::kNorthTeamId, false, true);

    const SkirmishMode::RetailState state = mode.GetRetailState();
    EXPECT_FALSE(state.overTime);
    EXPECT_EQ(state.nextLockDownTime, -1);
    EXPECT_EQ(state.teamWithOvertimeAdvantage, TeamMapping::kRetailNeutral);
    EXPECT_EQ(mode.GetOvertimeAdvantageTeam(), 0u);
}

TEST(SkirmishMode, RoundExpiryEntersRetailSuddenDeathAndClosesSpawns) {
    SkirmishMode mode(nullptr);
    mode.SetRoundTime(1.0f);
    mode.Initialize();
    AdvanceToActive(mode);

    mode.Update(1.0f);

    const SkirmishMode::RetailState state = mode.GetRetailState();
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::InstantDeath);
    EXPECT_TRUE(mode.IsInSuddenDeath());
    EXPECT_TRUE(state.suddenDeath);
    EXPECT_FALSE(state.overTime);
    EXPECT_EQ(state.spawnWindowCloseTime[0], 99999);
    EXPECT_EQ(state.spawnWindowCloseTime[1], 99999);
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kSouthTeamId));
    EXPECT_FALSE(mode.IsSpawnWindowOpen(SkirmishMode::kNorthTeamId));
}

TEST(SkirmishMode, RejectsMalformedTimeAndWrongPhaseEvents) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    mode.SetRoundTime(123.0f);
    mode.SetRoundTime(-1.0f);
    mode.SetRoundTime(std::numeric_limits<float>::quiet_NaN());
    mode.SetRoundTime(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(mode.GetRoundDuration(), 123.0f);

    mode.StartRound();
    ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
    const auto preparationCloseTimes = mode.GetSpawnWindowCloseTimes();
    mode.OnObjectiveCaptured(1, SkirmishMode::kSouthTeamId);
    mode.OnObjectiveControlChanged(SkirmishMode::kSouthTeamId, true, false);
    mode.OnTicketsDepleted(SkirmishMode::kSouthTeamId);
    mode.Update(-1.0f);
    mode.Update(std::numeric_limits<float>::quiet_NaN());
    mode.Update(std::numeric_limits<float>::infinity());
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Preparation);
    EXPECT_FLOAT_EQ(mode.GetPhaseTimeRemaining(), 15.0f);
    EXPECT_EQ(mode.GetSpawnWindowCloseTimes(), preparationCloseTimes);
    EXPECT_FALSE(mode.IsInOvertime());

    mode.Update(15.0f);
    ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
    EXPECT_FALSE(mode.CanReleaseSpawnWave(
        SkirmishMode::kSouthTeamId,
        std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(mode.CanReleaseSpawnWave(
        SkirmishMode::kSouthTeamId, -1.0f));
    mode.OnTicketsDepleted(99);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::Active);
}

TEST(SkirmishMode, RoundOutcomeAndRoundRecordAreOneShot) {
    SkirmishMode mode(nullptr);
    mode.Initialize();
    AdvanceToActive(mode);

    mode.OnTicketsDepleted(SkirmishMode::kSouthTeamId);
    ASSERT_EQ(mode.GetPhase(), SkirmishMode::Phase::PostRound);
    ASSERT_EQ(mode.GetTeamRoundWins(SkirmishMode::kNorthTeamId), 1);
    mode.OnTicketsDepleted(SkirmishMode::kNorthTeamId);
    mode.EndRound();
    EXPECT_EQ(mode.GetTeamRoundWins(SkirmishMode::kNorthTeamId), 1);
    EXPECT_EQ(mode.GetPlayedRoundsCount(), 1);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::NextRound);

    mode.EndRound();
    EXPECT_EQ(mode.GetPlayedRoundsCount(), 1);
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::NextRound);
}

TEST(SkirmishMode, ReinitializeRestoresMatchAndShortRoundState) {
    SkirmishMode mode(nullptr);
    mode.SetRoundTime(1.0f);
    mode.Initialize();
    mode.StartRound();
    EXPECT_EQ(mode.GetSpawnWindowCloseTimes()[TeamMapping::kRetailNva], -99);
    EXPECT_EQ(mode.GetSpawnWindowCloseTimes()[TeamMapping::kRetailUs], -99);
    mode.Update(15.0f);
    mode.OnTicketsDepleted(SkirmishMode::kSouthTeamId);
    ASSERT_EQ(mode.GetTeamRoundWins(SkirmishMode::kNorthTeamId), 1);

    mode.Initialize();
    EXPECT_EQ(mode.GetPhase(), SkirmishMode::Phase::WarmUp);
    EXPECT_EQ(mode.GetCurrentRound(), 0);
    EXPECT_EQ(mode.GetPlayedRoundsCount(), 0);
    EXPECT_EQ(mode.GetTeamRoundWins(SkirmishMode::kNorthTeamId), 0);
    EXPECT_FALSE(mode.IsInSuddenDeath());
    EXPECT_FALSE(mode.IsInOvertime());
}

RS2V_TEST_MAIN()
