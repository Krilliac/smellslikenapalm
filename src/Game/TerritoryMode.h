// src/Game/TerritoryMode.h
// RS2V Territory game mode — linear objective capture with attacker/defender roles

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include "Math/Vector3.h"

class GameServer;

class TerritoryMode {
public:
    enum class Phase : uint8_t {
        WarmUp,             // Pre-round warmup
        Preparation,        // Brief setup period
        Active,             // Main gameplay
        Overtime,           // Attackers contesting when time runs out
        Lockdown,           // Extended time after main timer expires
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
    uint32_t GetAttackingTeam() const { return m_attackingTeam; }
    uint32_t GetDefendingTeam() const { return m_defendingTeam; }
    int GetCurrentRound() const { return m_currentRound; }
    float GetRoundTimeRemaining() const;
    float GetOvertimeRemaining() const;
    int GetAttackerObjectivesCaptured(int round) const;
    int GetRoundTicketsRemaining(int round, uint32_t teamId) const;
    bool IsInOvertime() const { return m_phase == Phase::Overtime; }
    bool IsInSuddenDeath() const { return m_phase == Phase::SuddenDeath; }

    // Configuration
    void SetRoundTime(float seconds);
    void SetLockdownTime(float seconds);
    void SetPreparationTime(float seconds);
    void SetPostRoundTime(float seconds);
    void SetAttackerTickets(uint32_t tickets);
    void SetDefenderTickets(uint32_t tickets);
    void SetTicketsOnCapture(uint32_t tickets);

private:
    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    // Team assignments
    uint32_t m_attackingTeam = 1;
    uint32_t m_defendingTeam = 2;

    // Round tracking
    int m_currentRound = 0;                  // 0 = first round, 1 = second
    int m_objectivesCapturedRound[2] = {};   // Per-round capture count
    uint32_t m_ticketsRemainingRound[2][2] = {}; // [round][team] ticket snapshots

    // Timing
    float m_roundTime = 600.0f;              // 10 minutes default
    float m_lockdownTime = 600.0f;           // 10 minutes lockdown
    float m_preparationTime = 30.0f;
    float m_postRoundTime = 15.0f;
    float m_overtimeMaxTime = 120.0f;        // 2 minutes overtime
    float m_phaseTimer = 0.0f;

    // Ticket configuration
    uint32_t m_attackerStartTickets = 300;
    uint32_t m_defenderStartTickets = 200;
    uint32_t m_ticketsOnCapture = 25;        // Tickets refunded on objective capture

    void SetPhase(Phase newPhase);
    void CheckWinConditions();
    void CheckOvertimeConditions();
    void DetermineWinner();
    bool AreAttackersContesting() const;
    void SetupObjectives();
    void BroadcastPhaseChange() const;
};
