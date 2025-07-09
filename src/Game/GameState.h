// src/Game/GameState.h – Header for GameState

#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <memory>

class GameServer;
class GameMode;
class MapManager;
class PlayerManager;
class TeamManager;

enum class GamePhase {
    Waiting,        // Waiting for players
    Preparation,    // Pre-round preparation
    Active,         // Main gameplay
    PostRound,      // Round ending/results
    MapChanging     // Between maps
};

enum class MatchState {
    NotStarted,
    InProgress,
    Paused,
    Finished
};

struct ObjectiveState {
    uint32_t objectiveId;
    uint32_t controllingTeam;
    float captureProgress;
    bool isNeutral;
    std::chrono::steady_clock::time_point lastCaptureTime;
};

struct TeamScore {
    uint32_t teamId;
    uint32_t score;
    uint32_t kills;
    uint32_t deaths;
    uint32_t objectivesCaptured;
};

class GameState {
public:
    GameState(GameServer* server);
    ~GameState();

    void Initialize();
    void Update();
    void Reset();

    // Phase management
    void SetPhase(GamePhase phase);
    GamePhase GetPhase() const;
    void AdvancePhase();

    // Match management
    void StartMatch();
    void EndMatch();
    void PauseMatch();
    void ResumeMatch();
    MatchState GetMatchState() const;

    // Round management
    void StartRound();
    void EndRound();
    uint32_t GetCurrentRound() const;
    uint32_t GetMaxRounds() const;

    // Time management
    std::chrono::steady_clock::time_point GetRoundStartTime() const;
    std::chrono::steady_clock::time_point GetRoundEndTime() const;
    std::chrono::seconds GetRemainingTime() const;
    bool IsTimeExpired() const;

    // Objective management
    void AddObjective(uint32_t objectiveId, uint32_t initialTeam = 0);
    void UpdateObjective(uint32_t objectiveId, uint32_t teamId, float progress);
    void CaptureObjective(uint32_t objectiveId, uint32_t teamId);
    const std::vector<ObjectiveState>& GetObjectives() const;
    ObjectiveState* GetObjective(uint32_t objectiveId);

    // Score management
    void AddTeamScore(uint32_t teamId, uint32_t points);
    void SetTeamScore(uint32_t teamId, uint32_t score);
    uint32_t GetTeamScore(uint32_t teamId) const;
    void AddTeamKill(uint32_t teamId);
    void AddTeamDeath(uint32_t teamId);
    const std::vector<TeamScore>& GetTeamScores() const;

    // Win conditions
    bool CheckWinCondition();
    uint32_t GetWinningTeam() const;
    std::string GetWinReason() const;

    // State broadcasting
    void BroadcastGameState();
    void BroadcastPhaseChange();
    void BroadcastScoreUpdate();
    void BroadcastObjectiveUpdate(uint32_t objectiveId);

    // Serialization for network
    std::vector<uint8_t> SerializeGameState() const;
    void DeserializeGameState(const std::vector<uint8_t>& data);

private:
    GameServer* m_server;
    
    // Core state
    GamePhase m_currentPhase;
    MatchState m_matchState;
    uint32_t m_currentRound;
    uint32_t m_maxRounds;
    
    // Timing
    std::chrono::steady_clock::time_point m_roundStartTime;
    std::chrono::steady_clock::time_point m_roundEndTime;
    std::chrono::seconds m_roundDuration;
    std::chrono::steady_clock::time_point m_phaseStartTime;
    
    // Game objects
    std::vector<ObjectiveState> m_objectives;
    std::vector<TeamScore> m_teamScores;
    
    // Win condition tracking
    uint32_t m_winningTeam;
    std::string m_winReason;
    uint32_t m_scoreLimit;
    uint32_t m_objectiveLimit;
    
    // Helper methods
    void InitializeTeamScores();
    void InitializeObjectives();
    void UpdateTimers();
    void CheckPhaseTransition();
    TeamScore* GetTeamScoreRef(uint32_t teamId);
    void LogStateChange(const std::string& change);
};