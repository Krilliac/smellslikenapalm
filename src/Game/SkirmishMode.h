// src/Game/SkirmishMode.h
// RS2V Skirmish game mode — small-scale multi-round objective contest

#pragma once

#include <cstdint>
#include <string>

class GameServer;

class SkirmishMode {
public:
    enum class Phase : uint8_t {
        WarmUp,
        Preparation,
        Active,
        InstantDeath,       // 2-minute timer, no respawns
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

    // Events
    void OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam);
    void OnTicketsDepleted(uint32_t teamId);
    void OnPlayerKilled(uint32_t killerId, uint32_t victimId);

    // State queries
    Phase GetPhase() const { return m_phase; }
    int GetCurrentRound() const { return m_currentRound; }
    int GetMaxRounds() const { return m_maxRounds; }
    int GetTeamRoundWins(uint32_t teamId) const;
    float GetRoundTimeRemaining() const;
    float GetInstantDeathTimeRemaining() const;

    // Configuration
    void SetMaxRounds(int rounds);
    void SetRoundTime(float seconds);
    void SetInstantDeathTime(float seconds);
    void SetTicketsPerRound(uint32_t tickets);

private:
    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    int m_currentRound = 0;
    int m_maxRounds = 5;
    int m_team1Wins = 0;
    int m_team2Wins = 0;

    // Timing
    float m_roundTime = 300.0f;            // 5 minutes per round
    float m_instantDeathTime = 120.0f;     // 2 minutes instant death
    float m_preparationTime = 15.0f;
    float m_postRoundTime = 10.0f;
    float m_phaseTimer = 0.0f;

    // Tickets
    uint32_t m_ticketsPerRound = 30;

    void SetPhase(Phase newPhase);
    void CheckWinConditions();
    void CheckInstantDeathWinner();
    void AwardRoundWin(uint32_t teamId);
    void DetermineMatchWinner();
    void BroadcastPhaseChange() const;
};
