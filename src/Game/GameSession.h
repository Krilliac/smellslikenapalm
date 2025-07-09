// src/Game/GameSession.h – Header for GameSession

#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <memory>

class Player;
class TeamManager;
class MapManager;
class GameMode;
class NetworkManager;

struct SessionPlayerInfo {
    uint32_t playerId;
    std::string playerName;
    uint32_t teamId;
    bool isAlive;
    std::chrono::steady_clock::time_point lastSpawnTime;
};

class GameSession {
public:
    GameSession(NetworkManager* network,
                TeamManager* teams,
                MapManager* maps,
                GameMode* mode);
    ~GameSession();

    void StartSession();
    void EndSession();
    void Update();  // Called each server tick
    void HandlePlayerConnect(uint32_t playerId);
    void HandlePlayerDisconnect(uint32_t playerId);
    void HandlePlayerDeath(uint32_t playerId);
    void HandlePlayerSpawn(uint32_t playerId);

    const std::vector<SessionPlayerInfo>& GetPlayers() const;
    uint32_t GetActivePlayerCount() const;
    uint32_t GetTeamPlayerCount(uint32_t teamId) const;

private:
    void InitializePlayers();
    void RespawnLogic();
    void BroadcastSessionState();

    NetworkManager*       m_network;
    TeamManager*          m_teamManager;
    MapManager*           m_mapManager;
    GameMode*             m_gameMode;

    std::vector<SessionPlayerInfo> m_players;
    std::chrono::steady_clock::time_point m_sessionStart;
    bool m_sessionActive = false;

    const std::chrono::seconds m_respawnDelay{10};
};