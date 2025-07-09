// src/Game/GameSession.cpp – Implementation for GameSession

#include "Game/GameSession.h"
#include "Network/NetworkManager.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Game/GameMode.h"
#include "Utils/Logger.h"

GameSession::GameSession(NetworkManager* network,
                         TeamManager* teams,
                         MapManager* maps,
                         GameMode* mode)
    : m_network(network),
      m_teamManager(teams),
      m_mapManager(maps),
      m_gameMode(mode)
{
    Logger::Info("GameSession constructed");
}

GameSession::~GameSession()
{
    EndSession();
}

void GameSession::StartSession()
{
    Logger::Info("Starting game session");
    m_sessionStart = std::chrono::steady_clock::now();
    m_sessionActive = true;
    InitializePlayers();
    m_gameMode->OnStart();
}

void GameSession::EndSession()
{
    if (!m_sessionActive) return;
    Logger::Info("Ending game session");
    m_gameMode->OnEnd();
    m_sessionActive = false;
}

void GameSession::InitializePlayers()
{
    m_players.clear();
    for (auto& conn : m_network->GetAllConnections()) {
        SessionPlayerInfo info;
        info.playerId = conn->GetClientId();
        info.playerName = conn->GetPlayerName();
        info.teamId = conn->GetTeamId();
        info.isAlive = false;
        info.lastSpawnTime = m_sessionStart;
        m_players.push_back(info);
    }
    Logger::Info("Initialized %zu players in session", m_players.size());
}

void GameSession::HandlePlayerConnect(uint32_t playerId)
{
    Logger::Info("Player %u connected - adding to session", playerId);
    SessionPlayerInfo info;
    info.playerId = playerId;
    info.playerName = m_network->GetConnection(playerId)->GetPlayerName();
    info.teamId = m_network->GetConnection(playerId)->GetTeamId();
    info.isAlive = false;
    info.lastSpawnTime = std::chrono::steady_clock::now();
    m_players.push_back(info);
}

void GameSession::HandlePlayerDisconnect(uint32_t playerId)
{
    Logger::Info("Player %u disconnected - removing from session", playerId);
    m_players.erase(
        std::remove_if(m_players.begin(), m_players.end(),
            [&](const SessionPlayerInfo& info){ return info.playerId == playerId; }),
        m_players.end());
}

void GameSession::HandlePlayerDeath(uint32_t playerId)
{
    for (auto& info : m_players) {
        if (info.playerId == playerId) {
            info.isAlive = false;
            info.lastSpawnTime = std::chrono::steady_clock::now();
            Logger::Debug("Player %u died - will respawn after delay", playerId);
            break;
        }
    }
}

void GameSession::HandlePlayerSpawn(uint32_t playerId)
{
    for (auto& info : m_players) {
        if (info.playerId == playerId) {
            info.isAlive = true;
            info.lastSpawnTime = std::chrono::steady_clock::now();
            Logger::Debug("Player %u spawned", playerId);
            break;
        }
    }
}

void GameSession::Update()
{
    if (!m_sessionActive) return;

    // Respawn logic
    RespawnLogic();

    // Broadcast state periodically (could be per tick or less frequent)
    BroadcastSessionState();
}

void GameSession::RespawnLogic()
{
    auto now = std::chrono::steady_clock::now();
    for (auto& info : m_players) {
        if (!info.isAlive) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info.lastSpawnTime);
            if (elapsed >= m_respawnDelay) {
                HandlePlayerSpawn(info.playerId);
                m_network->GetConnection(info.playerId)->SendSpawnPlayer();
            }
        }
    }
}

void GameSession::BroadcastSessionState()
{
    // Example: send alive counts to all clients
    uint32_t aliveCount = GetActivePlayerCount();
    for (auto& conn : m_network->GetAllConnections()) {
        conn->SendSessionState(aliveCount);
    }
}

const std::vector<SessionPlayerInfo>& GameSession::GetPlayers() const
{
    return m_players;
}

uint32_t GameSession::GetActivePlayerCount() const
{
    return std::count_if(m_players.begin(), m_players.end(),
                         [](const SessionPlayerInfo& info){ return info.isAlive; });
}

uint32_t GameSession::GetTeamPlayerCount(uint32_t teamId) const
{
    return std::count_if(m_players.begin(), m_players.end(),
                         [&](const SessionPlayerInfo& info){
                             return info.isAlive && info.teamId == teamId;
                         });
}