// src/Game/PlayerManager.cpp – Implementation for PlayerManager

#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include "Utils/PacketAnalysis.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Game/TicketSystem.h"
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

    // Clear the player's TeamManager membership too, else m_playerTeamMap / team
    // playerIds keep a ghost entry, inflating GetTeamSize and skewing auto-balance for
    // future joins (TeamManager::RemovePlayer is otherwise only called from AddPlayerToTeam).
    if (m_server) { if (auto* tm = m_server->GetTeamManager()) tm->RemovePlayer(clientId); }
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

        // Handle respawns. Gate strictly on Dead (NOT Spectating) AND a real team:
        // !IsAlive() matches both Dead and Spectating, and a fresh/menu player is Dead with
        // no team - so the old time-only check force-promoted spectators and never-joined
        // players to Alive after the respawn delay. Only respawn a player who has actually
        // joined a team and died.
        if (pl->GetState() == PlayerState::Dead && pl->GetTeam() != 0 &&
            pl->IsReadyToSpawn() && pl->CanRespawn(m_respawnDelaySec)) {
            // Block respawn once the team has exhausted its reinforcement tickets,
            // mirroring ROGameInfo.PlayerShouldRespawn (returns false when
            // Team.ReinforcementsRemaining <= 0 for ticket-based modes). The per-death
            // cost is already debited via TicketSystem::OnPlayerKilled, so we ONLY gate
            // here - adding a per-spawn decrement would double-count the death. A team
            // with no ticket pool (GetInitialTickets == 0, e.g. an unlimited/Supremacy
            // config) is never gated, preserving the prior unlimited-respawn behavior.
            auto* ts = m_server->GetTicketSystem();
            const uint32_t team = pl->GetTeam();
            const bool outOfReinforcements =
                ts && ts->GetInitialTickets(team) > 0 && !ts->HasTickets(team);
            if (outOfReinforcements) {
                Logger::Debug("PlayerManager: Player %u ready to respawn but team %u is out "
                              "of reinforcements; staying down", id, team);
            } else {
                OnPlayerSpawn(id);
            }
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
        if (m_server) { if (auto* tm = m_server->GetTeamManager()) tm->RemovePlayer(id); }
        m_players.erase(id);
        m_playerStats.erase(id);
        Logger::Info("PlayerManager: Removed stale player %u", id);
    }
}

// Statistics tracking
int PlayerManager::GetPlayerKills(uint32_t clientId) const {
    auto it = m_playerStats.find(clientId);
    return it != m_playerStats.end() ? it->second.kills : 0;
}

int PlayerManager::GetPlayerDeaths(uint32_t clientId) const {
    auto it = m_playerStats.find(clientId);
    return it != m_playerStats.end() ? it->second.deaths : 0;
}

int PlayerManager::GetPlayerScore(uint32_t clientId) const {
    auto it = m_playerStats.find(clientId);
    return it != m_playerStats.end() ? it->second.score : 0;
}

void PlayerManager::SetPlayerKills(uint32_t clientId, int kills) {
    m_playerStats[clientId].kills = kills;
}

void PlayerManager::SetPlayerDeaths(uint32_t clientId, int deaths) {
    m_playerStats[clientId].deaths = deaths;
}

void PlayerManager::SetPlayerScore(uint32_t clientId, int score) {
    m_playerStats[clientId].score = score;
}

void PlayerManager::AddPlayerKill(uint32_t clientId) {
    m_playerStats[clientId].kills++;
}

void PlayerManager::AddPlayerDeath(uint32_t clientId) {
    m_playerStats[clientId].deaths++;
}

void PlayerManager::AddPlayerScore(uint32_t clientId, int points) {
    m_playerStats[clientId].score += points;
}