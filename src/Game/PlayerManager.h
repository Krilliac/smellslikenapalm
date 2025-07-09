// src/Game/PlayerManager.h

#pragma once

#include <unordered_map>
#include <memory>
#include <vector>
#include "Game/Player.h"
#include "Network/ClientConnection.h"

class GameServer;

class PlayerManager {
public:
    explicit PlayerManager(GameServer* server);
    ~PlayerManager();

    // Lifecycle
    void Initialize();
    void Shutdown();

    // Player events
    void OnPlayerConnect(std::shared_ptr<ClientConnection> conn);
    void OnPlayerDisconnect(uint32_t clientId);
    void OnPlayerDeath(uint32_t clientId);
    void OnPlayerSpawn(uint32_t clientId);

    // Per-tick update (now handles weapon timers and firing)
    void Update();

    // Accessors
    std::shared_ptr<Player> GetPlayer(uint32_t clientId) const;
    std::vector<std::shared_ptr<Player>> GetAllPlayers() const;
    std::vector<std::shared_ptr<Player>> GetDeadPlayers() const;
    std::vector<std::shared_ptr<Player>> GetAlivePlayers() const;

    // Utility
    uint32_t FindPlayerBySteamID(const std::string& steamId) const;
    void BroadcastPlayerList() const;

private:
    GameServer* m_server;
    std::unordered_map<uint32_t, std::shared_ptr<Player>> m_players;

    // Respawn settings
    int m_respawnDelaySec{10};

    // Cleans up disconnected players and dumps their last packets
    void RemoveStalePlayers();
};