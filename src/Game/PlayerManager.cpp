// src/Game/PlayerManager.cpp – Implementation for PlayerManager

#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include "Utils/PacketAnalysis.h"
#include "Game/GameServer.h"
#include "Game/SkirmishMode.h"
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
    // Keep ownership symmetric with OnPlayerConnect even when this subsystem is
    // shut down independently while GameServer remains alive. GameServer's
    // normal teardown may already have destroyed these systems; its accessors
    // and RemoveCombatParticipant both tolerate that partial state.
    if (m_server) {
        TeamManager* teams = m_server->GetTeamManager();
        for (const auto& [id, player] : m_players) {
            (void)player;
            if (teams) teams->RemovePlayer(id);
            m_server->RemoveCombatParticipant(id);
        }
    }
    m_players.clear();
    m_playerStats.clear();
}

void PlayerManager::OnPlayerConnect(std::shared_ptr<ClientConnection> conn)
{
    if (!conn) {
        Logger::Warn("PlayerManager: Ignoring null player connection");
        return;
    }

    // Some UE3 lifecycle callbacks are emitted after a decoded control bunch
    // without retaining the original datagram on ClientConnection. An absent
    // diagnostic sample is normal, not a malformed packet.
    if (!conn->LastRawPacket().empty()) {
        DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerConnect");
    }

    uint32_t id = conn->GetClientId();
    std::shared_ptr<Player> player = std::make_shared<Player>(id, conn);
    player->Initialize(conn->GetPlayerName(), conn->GetTeamId());
    m_players[id] = player;
    if (m_server) m_server->RegisterCombatParticipant(id);
    Logger::Info("PlayerManager: Player %u connected", id);
    BroadcastPlayerList();
}

void PlayerManager::OnPlayerDisconnect(uint32_t clientId)
{
    if (m_server) {
        if (auto conn = m_server->GetClientConnection(clientId);
            conn && !conn->LastRawPacket().empty()) {
            DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerDisconnect");
        }
    }

    // Clear the player's TeamManager membership too, else m_playerTeamMap / team
    // playerIds keep a ghost entry, inflating GetTeamSize and skewing auto-balance for
    // future joins (TeamManager::RemovePlayer is otherwise only called from AddPlayerToTeam).
    if (m_server) { if (auto* tm = m_server->GetTeamManager()) tm->RemovePlayer(clientId); }
    if (m_server) m_server->RemoveCombatParticipant(clientId);
    m_players.erase(clientId);
    m_playerStats.erase(clientId);
    Logger::Info("PlayerManager: Player %u disconnected", clientId);
    BroadcastPlayerList();
}

void PlayerManager::OnPlayerDeath(uint32_t clientId)
{
    auto pl = GetPlayer(clientId);
    if (!pl) {
        Logger::Warn("PlayerManager: Ignoring death for unknown player %u", clientId);
        return;
    }

    // Player::SetHealth(0) already transitions to Dead and records m_deathTime.
    // DamageSystem and the combat-authority bridge then notify us via this method.
    // Treat that notification (and any retransmit/duplicate) as a no-op instead of
    // recording a second, later timestamp that silently extends the respawn delay.
    // Spectators are likewise not valid death-transition sources.
    if (pl->GetState() != PlayerState::Alive) {
        Logger::Trace("PlayerManager: Ignoring duplicate/non-alive death for player %u "
                      "(state=%d)", clientId, static_cast<int>(pl->GetState()));
        return;
    }

    if (m_server) {
        if (auto conn = m_server->GetClientConnection(clientId)) {
            DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerDeath");
        }
    }

    // Keep the state/health invariant coherent for direct death notifications.
    // SetHealth performs both the Dead transition and the single timer stamp. The
    // fallback handles a pre-existing zero-health-but-Alive inconsistent state.
    if (pl->GetHealth() > 0) {
        pl->SetHealth(0);
    } else {
        pl->SetState(PlayerState::Dead);
        pl->MarkDeath();
    }
    Logger::Debug("PlayerManager: Player %u died", clientId);
}

void PlayerManager::OnPlayerSpawn(uint32_t clientId)
{
    auto pl = GetPlayer(clientId);
    if (!pl) {
        Logger::Warn("PlayerManager: Ignoring spawn for unknown player %u", clientId);
        return;
    }

    // Spawns are one-way transitions. A duplicate must not refill health or re-run
    // combat-authority respawn side effects, and a spectator must remain a spectator.
    if (pl->GetState() != PlayerState::Dead) {
        Logger::Trace("PlayerManager: Ignoring duplicate/non-dead spawn for player %u "
                      "(state=%d)", clientId, static_cast<int>(pl->GetState()));
        return;
    }

    if (m_server) {
        if (auto conn = m_server->GetClientConnection(clientId);
            conn && !conn->LastRawPacket().empty()) {
            DumpPacketForAnalysis(conn->LastRawPacket(), "OnPlayerSpawn");
        }
    }

    pl->SetState(PlayerState::Alive);
    pl->SetHealth(100);
    if (m_server) m_server->RespawnCombatParticipant(clientId);
    Logger::Debug("PlayerManager: Player %u spawned", clientId);
}

void PlayerManager::Update()
{
    if (!m_server) {
        Logger::Warn("PlayerManager: Cannot update without a GameServer");
        return;
    }
    const auto serverConfig = m_server->GetServerConfig();
    if (!serverConfig || serverConfig->GetTickRate() <= 0) {
        Logger::Error("PlayerManager: Cannot update with a missing/non-positive tick rate");
        return;
    }
    float deltaSeconds = 1.0f / static_cast<float>(serverConfig->GetTickRate());
    for (auto& [id, pl] : m_players) {
        // A null entry should be impossible through the public insertion path,
        // but leave it for RemoveStalePlayers to scrub rather than crashing the
        // entire server tick if state is corrupted or a future path violates the
        // invariant.
        if (!pl) continue;
        auto conn = pl->GetConnection();
        // Only dump when there is an actual packet. This runs every tick for
        // every player; LastRawPacket() is usually empty, and dumping an empty
        // buffer made PacketAnalyzer log "empty packet received" tens of times a
        // second per player (tens of thousands of lines in one session).
        if (conn && !conn->LastRawPacket().empty()) {
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
            } else if (m_server->GetSkirmishMode()) {
                // Skirmish casualties are released only by SpawnSystem's
                // synchronized 25-second team waves. Promoting them here on the
                // generic per-player delay bypasses the finite retail windows.
                Logger::Trace("PlayerManager: Player %u is waiting for team %u's "
                              "Skirmish deployment wave", id, team);
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
        if (pl && !pl->IsAlive()) list.push_back(pl);
    }
    return list;
}

std::vector<std::shared_ptr<Player>> PlayerManager::GetAlivePlayers() const
{
    std::vector<std::shared_ptr<Player>> list;
    for (auto& [id, pl] : m_players) {
        if (pl && pl->IsAlive()) list.push_back(pl);
    }
    return list;
}

uint32_t PlayerManager::FindPlayerBySteamID(const std::string& steamId) const
{
    for (auto& [id, pl] : m_players) {
        if (pl && pl->GetConnection() && pl->GetConnection()->GetSteamID() == steamId) {
            return id;
        }
    }
    return UINT32_MAX;
}

void PlayerManager::BroadcastPlayerList() const
{
    if (!m_server) {
        Logger::Trace("PlayerManager: No GameServer; skipping player-list broadcast");
        return;
    }

    std::string msg = "Current players:";
    for (auto& [id, pl] : m_players) {
        if (pl && pl->GetConnection()) {
            msg += " " + pl->GetConnection()->GetPlayerName();
        }
    }
    m_server->BroadcastChatMessage(msg);
}

void PlayerManager::SpawnAllPlayers()
{
    for (auto& [id, pl] : m_players) {
        if (pl && !pl->IsAlive()) {
            OnPlayerSpawn(id);
        }
    }
}

void PlayerManager::RemoveStalePlayers()
{
    std::vector<uint32_t> toRemove;
    for (auto& [id, pl] : m_players) {
        const auto conn = pl ? pl->GetConnection() : nullptr;
        if (!pl || !conn || conn->IsDisconnected()) {
            if (conn) {
                DumpPacketForAnalysis(conn->LastRawPacket(), "RemoveStalePlayers");
            }
            toRemove.push_back(id);
        }
    }
    for (auto id : toRemove) {
        if (m_server) { if (auto* tm = m_server->GetTeamManager()) tm->RemovePlayer(id); }
        if (m_server) m_server->RemoveCombatParticipant(id);
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
