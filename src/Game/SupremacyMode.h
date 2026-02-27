// src/Game/SupremacyMode.h
// RS2V Supremacy game mode — tug-of-war objective control with point flow

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

class GameServer;

class SupremacyMode {
public:
    enum class Phase : uint8_t {
        WarmUp,
        Preparation,
        Active,
        SuddenDeath,
        PostRound,
        Finished
    };

    explicit SupremacyMode(GameServer* server);
    ~SupremacyMode();

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
    float GetRoundTimeRemaining() const;
    float GetTeamPoints(uint32_t teamId) const;
    float GetPointBarProgress() const;  // 0.0 = team1 losing, 0.5 = even, 1.0 = team2 losing
    int GetTeamObjectiveValue(uint32_t teamId) const;
    bool IsObjectiveLinked(uint32_t objectiveId, uint32_t teamId) const;

    // Configuration
    void SetStartingPoints(float points);
    void SetPointDrainInterval(float seconds);
    void SetRoundTime(float seconds);

    // Objective supply chain — which objectives link to which
    void SetObjectiveLinks(const std::map<uint32_t, std::vector<uint32_t>>& links);
    void SetTeamHQ(uint32_t teamId, uint32_t objectiveId);

private:
    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    // Points (tug-of-war bar)
    float m_team1Points = 250.0f;
    float m_team2Points = 250.0f;
    float m_startingPoints = 250.0f;
    float m_pointDrainInterval = 5.0f;    // Every 5 seconds
    float m_drainTimer = 0.0f;

    // Timing
    float m_roundTime = 1200.0f;           // 20 minutes
    float m_phaseTimer = 0.0f;
    float m_preparationTime = 30.0f;
    float m_postRoundTime = 15.0f;

    // Objective supply chain
    struct ObjectiveNode {
        uint32_t id = 0;
        int pointValue = 1;                // Objective value for scoring
        uint32_t controllingTeam = 0;
        std::vector<uint32_t> linkedTo;    // Adjacent objectives
    };
    std::map<uint32_t, ObjectiveNode> m_objectiveGraph;
    uint32_t m_team1HQ = 0;
    uint32_t m_team2HQ = 0;

    void SetPhase(Phase newPhase);
    void ProcessPointDrain(float deltaSeconds);
    int CalculateLinkedObjectiveValue(uint32_t teamId) const;
    bool HasPathToHQ(uint32_t objectiveId, uint32_t teamId,
                     std::vector<uint32_t>& visited) const;
    void CheckWinConditions();
    void DetermineWinner();
    void BroadcastPhaseChange() const;
};
