// src/Game/PlayerManager.cpp – Implementation for PlayerManager

#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include "Utils/PacketAnalysis.h"
#include "Game/GameServer.h"
#include "Config/ServerConfig.h"
#include "Network/ClientConnection.h"
#include <chrono>

PlayerManager::PlayerManager(GameServer* server)
    : m_server(server)
{
    Logger::Info("PlayerManager initialized");
}

PlayerManager::~PlayerManager()
{
    Shutdown();
}

void PlayerManager::Initialize()
{
    Logger::Info("PlayerManager: Ready");
}

void PlayerManager::Shutdown()
{
    Logger::Info("PlayerManager: Shutting down, clearing players");
    m_players.clear();
}

void PlayerManager::OnPlayerConnect(std::shared_ptr<ClientConnection> conn)
{
    DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerConnect");

    uint32_t id = conn->GetClientId();
    auto player = std::make_shared<Player>(id, conn);
    player->Initialize(conn->GetPlayerName(), conn->GetTeamId());
    m_players[id] = player;
    Logger::Info("PlayerManager: Player %u connected", id);
    BroadcastPlayerList();
}

void PlayerManager::OnPlayerDisconnect(uint32_t clientId)
{
    if (auto conn = m_server->GetClientConnection(clientId)) {
        DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerDisconnect");
    }

    m_players.erase(clientId);
    Logger::Info("PlayerManager: Player %u disconnected", clientId);
    BroadcastPlayerList();
}

void PlayerManager::OnPlayerDeath(uint32_t clientId)
{
    if (auto conn = m_server->GetClientConnection(clientId)) {
        DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerDeath");
    }

    auto pl = GetPlayer(clientId);
    if (!pl) return;
    pl->SetState(PlayerState::Dead);
    pl->MarkDeath();
    Logger::Debug("PlayerManager: Player %u died", clientId);
}

void PlayerManager::OnPlayerSpawn(uint32_t clientId)
{
    if (auto conn = m_server->GetClientConnection(clientId)) {
        DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerSpawn");
    }

    auto pl = GetPlayer(clientId);
    if (!pl) return;
    pl->SetState(PlayerState::Alive);
    pl->SetHealth(100);
    Logger::Debug("PlayerManager: Player %u spawned", clientId);
}

void PlayerManager::Update()
{
    float deltaSeconds = 1.0f / m_server->GetServerConfig()->GetTickRate();
    for (auto& [id, pl] : m_players) {
        auto conn = pl->GetConnection();
        if (conn) {
            DumpPacketForAnalysis(conn->LastRawPacket(), "Update_PlayerTelemetry");
        }

        // Handle respawns
        if (!pl->IsAlive() && pl->CanRespawn(m_respawnDelaySec)) {
            OnPlayerSpawn(id);
        }
        pl->Update(deltaSeconds);
    }
    RemoveStalePlayers();
}

std::shared_ptr<Player> PlayerManager::GetPlayer(uint32_t clientId) const
{
    auto it = m_players.find(clientId);
    return it != m_players.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<Player>> PlayerManager::GetAllPlayers() const
{
    std::vector<std::shared_ptr<Player>> list;
    list.reserve(m_players.size());
    for (auto& [id, pl] : m_players) list.push_back(pl);
    return list;
}

std::vector<std::shared_ptr<Player>> PlayerManager::GetDeadPlayers() const
{
    std::vector<std::shared_ptr<Player>> list;
    for (auto& [id, pl] : m_players) {
        if (!pl->IsAlive()) list.push_back(pl);
    }
    return list;
}

std::vector<std::shared_ptr<Player>> PlayerManager::GetAlivePlayers() const
{
    std::vector<std::shared_ptr<Player>> list;
    for (auto& [id, pl] : m_players) {
        if (pl->IsAlive()) list.push_back(pl);
    }
    return list;
}

uint32_t PlayerManager::FindPlayerBySteamID(const std::string& steamId) const
{
    for (auto& [id, pl] : m_players) {
        if (pl->GetConnection()->GetSteamID() == steamId) {
            return id;
        }
    }
    return UINT32_MAX;
}

void PlayerManager::BroadcastPlayerList() const
{
    std::string msg = "Current players:";
    for (auto& [id, pl] : m_players) {
        msg += " " + pl->GetConnection()->GetPlayerName();
    }
    m_server->BroadcastChatMessage(msg);
}

void PlayerManager::SpawnAllPlayers()
{
    for (auto& [id, pl] : m_players) {
        if (!pl->IsAlive()) {
            OnPlayerSpawn(id);
        }
    }
}

void PlayerManager::RemoveStalePlayers()
{
    std::vector<uint32_t> toRemove;
    for (auto& [id, pl] : m_players) {
        if (pl->GetConnection()->IsDisconnected()) {
            DumpPacketForAnalysis(pl->GetConnection()->LastRawPacket(), "RemoveStalePlayers");
            toRemove.push_back(id);
        }
    }
    for (auto id : toRemove) {
        m_players.erase(id);
        Logger::Info("PlayerManager: Removed stale player %u", id);
    }
}