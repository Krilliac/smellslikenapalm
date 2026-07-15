// src/Game/SkirmishMode.h
// RS2V Skirmish game mode - wave-limited multi-round objective contest.

#pragma once

#include "Game/TeamMapping.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

class GameServer;

class SkirmishMode {
public:
    static constexpr uint32_t kSouthTeamId = TeamMapping::kServerUs;
    static constexpr uint32_t kNorthTeamId = TeamMapping::kServerNva;
    static constexpr int32_t kSpawnWaveCount = 5;
    static constexpr int32_t kSpawnWaveIntervalSeconds = 25;
    static constexpr int32_t kMaxSpawnWindowSeconds =
        (kSpawnWaveCount - 1) * kSpawnWaveIntervalSeconds;
    static constexpr int32_t kAllObjectiveLockdownSeconds = 60;
    static constexpr int32_t kDefaultRoundTeamScoreLimit = 10;
    static constexpr std::array<int32_t, kSpawnWaveCount> kSpawnWindowOffsetsSeconds = {
        100, 75, 50, 25, 0
    };

    // Directly consumable values for ROGameReplicationInfo. Arrays use retail
    // order: [0] North/NVA, [1] South/US. Unused h37 entries and inactive h67
    // use retail's -1 sentinel; h126 uses 2 for no team/neutral.
    struct RetailState {
        std::array<int32_t, 20> allSpawnWindows{}; // h37
        std::array<int32_t, 2> spawnWindowCloseTime{}; // h38
        int32_t playedRoundsCount = 0; // h39
        int32_t roundTeamScoreLimit = kDefaultRoundTeamScoreLimit; // h40
        int32_t roundLimit = 5; // h41
        int32_t nextLockDownTime = -1; // h67
        bool suddenDeath = false; // h116
        bool overTime = false; // h117 (all-objective lockdown in Skirmish)
        uint8_t teamWithOvertimeAdvantage = TeamMapping::kRetailNeutral; // h126
        std::array<uint8_t, 2> playersAliveCount{}; // h129
    };

    enum class Phase : uint8_t {
        WarmUp,
        Preparation,
        Active,
        InstantDeath,       // Retail bSuddenDeath: no further spawn windows
        PostRound,
        NextRound,
        Finished
    };

    explicit SkirmishMode(GameServer* server);
    ~SkirmishMode();

    void Initialize();
    void Shutdown();

    void StartRound();
    void EndRound();
    void Update(float deltaSeconds);

    // Events. Objective-control input must come from the authoritative
    // ObjectiveSystem, never from a client assertion.
    void OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam);
    void OnObjectiveControlChanged(uint32_t controllingTeam,
                                   bool controlsAllObjectives,
                                   bool objectiveContested);
    void OnTicketsDepleted(uint32_t teamId);
    void OnPlayerKilled(uint32_t killerId, uint32_t victimId);

    // State queries
    Phase GetPhase() const { return m_phase; }
    bool CanCaptureObjectives() const {
        return m_phase == Phase::Active || m_phase == Phase::InstantDeath;
    }
    int GetCurrentRound() const { return m_currentRound; }
    int GetPlayedRoundsCount() const { return m_playedRoundsCount; }
    int GetMaxRounds() const { return m_maxRounds; }
    int GetRoundTeamScoreLimit() const { return m_roundTeamScoreLimit; }
    int GetTeamRoundWins(uint32_t teamId) const;
    float GetRoundDuration() const { return m_roundTime; }
    float GetRoundTimeRemaining() const;
    float GetPhaseDuration() const;
    float GetPhaseTimeRemaining() const;
    float GetInstantDeathTimeRemaining() const;
    float GetLockdownTimeRemaining() const;
    RetailState GetRetailState() const;
    std::array<int32_t, 20> GetAllSpawnWindows() const;
    const std::array<int32_t, 2>& GetSpawnWindowCloseTimes() const {
        return m_spawnWindowCloseTime;
    }
    int32_t GetNextLockdownTime() const { return m_nextLockdownTime; }
    bool IsInSuddenDeath() const { return m_suddenDeath; }
    bool IsInOvertime() const { return m_overtime; }
    // Server-team id (1=South/US, 2=North/NVA); GetRetailState converts h126.
    uint32_t GetOvertimeAdvantageTeam() const { return m_overtimeAdvantageTeam; }
    uint8_t GetPlayersAliveCount(uint32_t teamId) const;
    const std::array<uint8_t, 2>& GetPlayersAliveCountsRetail() const {
        return m_playersAliveCount;
    }
    bool IsSpawnWindowOpen(uint32_t teamId) const;
    // Evaluate a wave that SpawnSystem will release during the current tick.
    // Uses the same post-delta retail countdown as Update and permits the final
    // h37 wave exactly at SpawnWindowCloseTime (the ordinary window query is
    // strict because no later respawns remain once that release has occurred).
    bool CanReleaseSpawnWave(uint32_t teamId, float deltaSeconds) const;
    int32_t GetNextSpawnWaveTime(uint32_t teamId) const;
    int32_t GetSpawnWavesRemaining(uint32_t teamId) const;

    // Configuration
    void SetMaxRounds(int rounds);
    void SetRoundTime(float seconds);
    void SetInstantDeathTime(float seconds);
    void SetTicketsPerRound(uint32_t tickets);

private:
    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    int m_currentRound = 0;
    int m_playedRoundsCount = 0;
    int m_maxRounds = 5;
    int m_roundTeamScoreLimit = kDefaultRoundTeamScoreLimit;
    int m_team1Wins = 0;
    int m_team2Wins = 0;
    uint32_t m_roundWinner = 0;
    bool m_roundRecorded = false;

    // Timing
    float m_roundTime = 300.0f;
    float m_instantDeathTime = 120.0f;
    float m_preparationTime = 15.0f;
    float m_postRoundTime = 10.0f;
    float m_phaseTimer = 0.0f;

    // Retail Skirmish mode state. Spawn close times and lockdown time are
    // absolute targets in the same decreasing RemainingTime clock as h29.
    std::array<int32_t, 2> m_spawnWindowCloseTime = {0, 0};
    std::array<uint8_t, 2> m_playersAliveCount = {0, 0};
    int32_t m_nextLockdownTime = -1;
    uint32_t m_overtimeAdvantageTeam = 0;
    bool m_suddenDeath = false;
    bool m_overtime = false;
    bool m_objectiveContested = false;
    bool m_enteredSuddenDeathInOvertime = false;
    std::unordered_map<uint32_t, uint32_t> m_lastObjectiveCaptureTeam;

    // Legacy ticket setting retained for source compatibility. Retail
    // Skirmish respawn availability is represented by the five wave windows.
    uint32_t m_ticketsPerRound = 30;

    void SetPhase(Phase newPhase);
    void ResetRoundRetailState();
    void OpenInitialSpawnWindows();
    void ExtendSpawnWindow(uint32_t teamId);
    void RefreshAliveCounts();
    void RefreshObjectiveState();
    void BeginSuddenDeath();
    void CancelObjectiveLockdown();
    bool CheckObjectiveLockdown();
    void CheckWinConditions();
    void CheckInstantDeathWinner();
    void FinishRoundWithWinner(uint32_t teamId);
    void AwardRoundWin(uint32_t teamId);
    void DetermineMatchWinner();
    void BroadcastPhaseChange() const;
    int32_t GetRetailRemainingTime() const;
    static bool IsPlayableTeam(uint32_t teamId);
    static std::size_t RetailTeamSlot(uint32_t teamId);
};
