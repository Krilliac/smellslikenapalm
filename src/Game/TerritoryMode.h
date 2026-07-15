// src/Game/TerritoryMode.h
// RS2V Territory game mode — linear objective capture with attacker/defender roles

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <chrono>
#include "Math/Vector3.h"

class GameServer;
class ObjectiveSystem;

struct TerritoryModeTestAccess;

class TerritoryMode {
public:
    enum class Phase : uint8_t {
        WarmUp,             // Pre-round warmup
        Preparation,        // Brief setup period
        Active,             // Main gameplay
        Overtime,           // Attackers contesting when time runs out
        Lockdown,           // Legacy compatibility only; retail uses an Active-phase target
        SuddenDeath,        // No respawning — last team standing
        PostRound,          // Results display
        HalfTime,           // Switching sides between rounds
        Finished            // Both rounds done
    };

    explicit TerritoryMode(GameServer* server);
    ~TerritoryMode();

    void Initialize();
    void Shutdown();

    // Round management
    void StartRound();
    void EndRound();
    void SwitchSides();    // Swap attacker/defender roles between rounds

    // Per-tick update
    void Update(float deltaSeconds);

    // Events from other systems
    void OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam);
    void OnTicketsDepleted(uint32_t teamId);
    void OnPlayerKilled(uint32_t killerId, uint32_t victimId);
    void OnAllPlayersDead(uint32_t teamId);

    // State queries
    Phase GetPhase() const { return m_phase; }
    bool CanCaptureObjectives() const {
        return m_phase == Phase::Active || m_phase == Phase::Overtime ||
               m_phase == Phase::Lockdown;
    }
    uint32_t GetAttackingTeam() const { return m_attackingTeam; }
    uint32_t GetDefendingTeam() const { return m_defendingTeam; }
    int GetCurrentRound() const { return m_currentRound; }
    float GetRoundTimeRemaining() const;
    // Total duration assigned when the current phase began.  Retail GRI timer
    // replication uses this as TimeLimit and m_phaseTimer as RemainingTime.
    float GetPhaseDuration() const;
    float GetOvertimeRemaining() const;
    int GetAttackerObjectivesCaptured(int round) const;
    uint32_t GetRoundTicketsRemaining(int round, uint32_t teamId) const;
    uint32_t GetWinningTeam() const { return m_winningTeam; }
    bool IsInOvertime() const { return m_phase == Phase::Overtime; }
    bool IsInSuddenDeath() const { return m_phase == Phase::SuddenDeath; }
    // Connection replication uses the old countdown coordinate to preserve a
    // partially served reinforcement delay when SetPhase installs a new one.
    Phase GetPreviousPhase() const { return m_previousPhase; }
    float GetPreviousPhaseRemainingAtTransition() const {
        return m_previousPhaseRemainingAtTransition;
    }

    struct RetailTimingState {
        int32_t nextLockdownTime = -1;
        int32_t nextEstimatedLockdownTime = -1;
        bool overtime = false;
    };
    RetailTimingState GetRetailTimingState() const;
    float GetRoundDuration() const { return m_roundTime; }
    float GetLockdownDuration() const { return m_lockdownTime; }
    float GetCaptureLockdownDelay() const { return m_captureLockdownDelay; }
    float GetCaptureAttemptLockdownDelay() const {
        return m_captureAttemptLockdownDelay;
    }
    float GetOvertimeDuration() const { return m_overtimeMaxTime; }

    // Configuration
    void SetRoundTime(float seconds);
    void SetLockdownTime(float seconds);
    void SetCaptureLockdownDelay(float seconds);
    void SetCaptureAttemptLockdownDelay(float seconds);
    void SetPreparationTime(float seconds);
    void SetPostRoundTime(float seconds);
    void SetAttackerTickets(uint32_t tickets);
    void SetDefenderTickets(uint32_t tickets);
    void SetTicketsOnCapture(uint32_t tickets);

private:
    friend struct TerritoryModeTestAccess;

    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    // Team assignments
    uint32_t m_attackingTeam = 1;
    uint32_t m_defendingTeam = 2;

    // Round tracking
    int m_currentRound = 0;                  // 0 = first round, 1 = second
    int m_objectivesCapturedRound[2] = {};   // Per-round capture count
    // Stable numeric-team snapshots: slot 0=server team 1/US, slot 1=team 2/NVA.
    // Halftime role swaps must never reinterpret historical ticket values.
    uint32_t m_ticketsRemainingRound[2][2] = {};
    uint32_t m_attackingTeamRound[2] = {};
    uint32_t m_winningTeam = 0;
    bool m_roundFinalized = false;
    std::unordered_set<uint32_t> m_capturedObjectiveIds;

    // Timing
    float m_roundTime = 600.0f;              // 10 minutes default
    float m_lockdownTime = 300.0f;            // Retail early-win countdown
    float m_captureLockdownDelay = 600.0f;    // No completed capture
    float m_captureAttemptLockdownDelay = 420.0f; // No new capture attempt
    float m_preparationTime = 30.0f;
    float m_postRoundTime = 15.0f;
    float m_overtimeMaxTime = 180.0f;        // Retail main-expiry overtime
    float m_phaseTimer = 0.0f;
    Phase m_previousPhase = Phase::WarmUp;
    float m_previousPhaseRemainingAtTransition = 0.0f;
    float m_lastCaptureTime = -1.0f;
    float m_lastCaptureAttempt = -1.0f;
    float m_nextLockdownTime = -1.0f;
    float m_nextEstimatedLockdownTime = -1.0f;
    bool m_lockdownObjectiveEligible = false;
    bool m_attackersWereContesting = false;

    // Ticket configuration
    uint32_t m_attackerStartTickets = 300;
    uint32_t m_defenderStartTickets = 200;
    uint32_t m_ticketsOnCapture = 25;        // Tickets refunded on objective capture

    void SetPhase(Phase newPhase);
    void AdvanceActivePhase(float deltaSeconds, bool attackersContesting,
                            bool lockdownObjectiveEligible);
    void SynchronizeLockdownEligibility(bool eligible,
                                        float baselineRemaining,
                                        bool forceReset = false);
    void ClearLockdownSchedule();
    void RecomputeEstimatedLockdown();
    bool ResolveEarlyLockdown(bool attackersContesting);
    void CheckWinConditions();
    bool TeamHasLivingParticipant(uint32_t teamId) const;
    void CheckSuddenDeathElimination();
    void DetermineWinner();
    bool AreAttackersContesting() const;
    bool HasEligibleLockdownObjective() const;
    static bool AnyAttackerInCurrentTerritoryPhase(
        const ObjectiveSystem& objectives,
        uint32_t attackingTeam,
        const std::function<bool(uint32_t)>& isAttackingPlayer);
    static bool CurrentTerritoryPhaseSupportsLockdown(
        const ObjectiveSystem& objectives, uint32_t defendingTeam);
    static int32_t ToRetailCountdown(float seconds);
    void SetupObjectives();
    void BroadcastPhaseChange() const;
};
