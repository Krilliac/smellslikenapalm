// src/Game/ScoreManager.h – Header for ScoreManager

#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <memory>

class GameServer;
class TeamManager;

struct TeamScore {
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint32_t objectivesCaptured = 0;
    uint32_t score = 0;
};

class ScoreManager {
public:
    explicit ScoreManager(GameServer* server);
    ~ScoreManager();

    // Initialization and shutdown
    void Initialize();
    void Shutdown();

    // Recording events
    void RecordKill(uint32_t teamId);
    void RecordDeath(uint32_t teamId);
    void RecordObjectiveCapture(uint32_t teamId, uint32_t points);

    // Score adjustments
    void AddPoints(uint32_t teamId, uint32_t points);
    void SetPoints(uint32_t teamId, uint32_t points);

    // Queries
    TeamScore GetTeamScore(uint32_t teamId) const;
    std::vector<uint32_t> GetTeamsByScore() const;

    // Reset
    void ResetAll();

    // Broadcast updates
    void BroadcastScores() const;

private:
    GameServer*                 m_server;
    std::shared_ptr<TeamManager> m_teamManager;
    std::map<uint32_t, TeamScore> m_scores;

    // Internal helper
    void EnsureTeamExists(uint32_t teamId);
};