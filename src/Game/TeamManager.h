// src/Game/TeamManager.h – Header for TeamManager

#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <memory>

class GameServer;
class Player;

struct TeamInfo {
    uint32_t teamId;
    std::string name;
    std::vector<uint32_t> playerIds;
    uint32_t score = 0;
    uint32_t objectivesCaptured = 0;
};

class TeamManager {
public:
    explicit TeamManager(GameServer* server);
    ~TeamManager();

    // Initialization & shutdown
    void Initialize();
    void Shutdown();

    // Player-team management
    void AddPlayerToTeam(uint32_t playerId, uint32_t teamId);
    void RemovePlayer(uint32_t playerId);
    uint32_t GetPlayerTeam(uint32_t playerId) const;
    std::vector<uint32_t> GetTeamPlayers(uint32_t teamId) const;
    std::vector<uint32_t> GetAllTeams() const;
    size_t GetTeamCount() const;

    // Team scoring
    void AddTeamScore(uint32_t teamId, uint32_t points);
    uint32_t GetTeamScore(uint32_t teamId) const;
    void ResetScores();

    // Objectives
    bool CaptureObjective(uint32_t teamId, uint32_t objectiveId);
    uint32_t GetObjectivesCaptured(uint32_t teamId) const;

    // Balance & utility
    bool HasEnoughPlayers() const;
    void AutoBalanceTeams();

private:
    GameServer* m_server;
    std::map<uint32_t, TeamInfo> m_teams;
    std::map<uint32_t, uint32_t> m_playerTeamMap; // playerId -> teamId

    void EnsureTeamExists(uint32_t teamId, const std::string& name = "");
};