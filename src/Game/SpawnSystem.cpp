// src/Game/SpawnSystem.cpp
// RS2V spawn system implementation

#include "Game/SpawnSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/RoleSystem.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cstdlib>

SpawnSystem::SpawnSystem(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[SpawnSystem::SpawnSystem] Entry, server=%p", static_cast<void*>(server));
    Logger::Trace("[SpawnSystem::SpawnSystem] Exit");
}

SpawnSystem::~SpawnSystem() {
    Logger::Trace("[SpawnSystem::~SpawnSystem] Entry");
    Shutdown();
    Logger::Trace("[SpawnSystem::~SpawnSystem] Exit");
}

void SpawnSystem::Initialize() {
    Logger::Trace("[SpawnSystem::Initialize] Entry");
    m_spawnLocations.clear();
    m_waveStates.clear();
    Logger::Debug("[SpawnSystem::Initialize] Cleared spawn locations and wave states");
    Logger::Info("SpawnSystem initialized");
    Logger::Trace("[SpawnSystem::Initialize] Exit");
}

void SpawnSystem::Shutdown() {
    Logger::Trace("[SpawnSystem::Shutdown] Entry");
    Logger::Info("[SpawnSystem::Shutdown] Shutting down, clearing %zu spawn locations and %zu wave states",
                 m_spawnLocations.size(), m_waveStates.size());
    m_spawnLocations.clear();
    m_waveStates.clear();
    Logger::Trace("[SpawnSystem::Shutdown] Exit");
}

uint32_t SpawnSystem::AddSpawnLocation(const SpawnLocation& loc) {
    Logger::Trace("[SpawnSystem::AddSpawnLocation] Entry, name=%s, type=%d, team=%u, pos=(%.1f, %.1f, %.1f)",
                  loc.name.c_str(), static_cast<int>(loc.type), loc.teamId,
                  loc.position.x, loc.position.y, loc.position.z);
    SpawnLocation l = loc;
    l.id = m_nextSpawnId++;
    m_spawnLocations[l.id] = l;
    Logger::Info("Spawn location added: '%s' (id=%u, type=%d, team=%u) at (%.1f, %.1f, %.1f)",
                 l.name.c_str(), l.id, static_cast<int>(l.type), l.teamId,
                 l.position.x, l.position.y, l.position.z);
    Logger::Debug("[SpawnSystem::AddSpawnLocation] Total spawn locations: %zu", m_spawnLocations.size());
    Logger::Trace("[SpawnSystem::AddSpawnLocation] Exit, return id=%u", l.id);
    return l.id;
}

void SpawnSystem::RemoveSpawnLocation(uint32_t id) {
    Logger::Trace("[SpawnSystem::RemoveSpawnLocation] Entry, id=%u", id);
    auto it = m_spawnLocations.find(id);
    if (it != m_spawnLocations.end()) {
        Logger::Debug("[SpawnSystem::RemoveSpawnLocation] Removing spawn location '%s' (id=%u)", it->second.name.c_str(), id);
        m_spawnLocations.erase(it);
    } else {
        Logger::Debug("[SpawnSystem::RemoveSpawnLocation] Spawn location id=%u not found", id);
    }
    Logger::Trace("[SpawnSystem::RemoveSpawnLocation] Exit");
}

SpawnLocation* SpawnSystem::GetSpawnLocation(uint32_t id) {
    Logger::Trace("[SpawnSystem::GetSpawnLocation] Entry, id=%u", id);
    auto it = m_spawnLocations.find(id);
    SpawnLocation* result = it != m_spawnLocations.end() ? &it->second : nullptr;
    Logger::Debug("[SpawnSystem::GetSpawnLocation] id=%u %s", id, result ? "found" : "not found");
    Logger::Trace("[SpawnSystem::GetSpawnLocation] Exit, return %s", result ? "valid pointer" : "nullptr");
    return result;
}

std::vector<const SpawnLocation*> SpawnSystem::GetAvailableSpawns(uint32_t playerId) const {
    Logger::Trace("[SpawnSystem::GetAvailableSpawns] Entry, playerId=%u", playerId);
    std::vector<const SpawnLocation*> result;
    auto* tm = m_server->GetTeamManager();
    uint32_t playerTeam = tm->GetPlayerTeam(playerId);
    Logger::Debug("[SpawnSystem::GetAvailableSpawns] Player %u is on team %u, checking %zu spawn locations",
                  playerId, playerTeam, m_spawnLocations.size());

    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.teamId != playerTeam) {
            Logger::Trace("[SpawnSystem::GetAvailableSpawns] Spawn %u: wrong team (%u != %u), skipping", id, loc.teamId, playerTeam);
            continue;
        }
        if (!loc.isActive || loc.isDestroyed) {
            Logger::Trace("[SpawnSystem::GetAvailableSpawns] Spawn %u: inactive or destroyed, skipping", id);
            continue;
        }
        if (loc.spawnCooldown > 0.0f) {
            Logger::Trace("[SpawnSystem::GetAvailableSpawns] Spawn %u: on cooldown (%.1fs), skipping", id, loc.spawnCooldown);
            continue;
        }

        // Squad leader spawns require special checks
        if (loc.type == SpawnType::SquadLeader) {
            if (!CanSpawnOnSquadLeader(playerId)) {
                Logger::Debug("[SpawnSystem::GetAvailableSpawns] Spawn %u: squad leader spawn not available for player %u", id, playerId);
                continue;
            }
        }

        result.push_back(&loc);
        Logger::Debug("[SpawnSystem::GetAvailableSpawns] Spawn %u '%s' available", id, loc.name.c_str());
    }
    Logger::Debug("[SpawnSystem::GetAvailableSpawns] Found %zu available spawns for player %u", result.size(), playerId);
    Logger::Trace("[SpawnSystem::GetAvailableSpawns] Exit, return %zu spawns", result.size());
    return result;
}

std::vector<const SpawnLocation*> SpawnSystem::GetTeamSpawns(uint32_t teamId) const {
    Logger::Trace("[SpawnSystem::GetTeamSpawns] Entry, teamId=%u", teamId);
    std::vector<const SpawnLocation*> result;
    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.teamId == teamId) result.push_back(&loc);
    }
    Logger::Debug("[SpawnSystem::GetTeamSpawns] Found %zu spawns for team %u", result.size(), teamId);
    Logger::Trace("[SpawnSystem::GetTeamSpawns] Exit, return %zu spawns", result.size());
    return result;
}

void SpawnSystem::UpdateSquadLeaderSpawns() {
    Logger::Trace("[SpawnSystem::UpdateSquadLeaderSpawns] Entry");
    // Remove stale squad leader spawns and update positions
    std::vector<uint32_t> toRemove;
    auto* pm = m_server->GetPlayerManager();
    int updatedCount = 0;
    int deactivatedCount = 0;

    for (auto& [id, loc] : m_spawnLocations) {
        if (loc.type != SpawnType::SquadLeader) continue;

        auto leader = pm->GetPlayer(loc.squadLeaderId);
        if (!leader || !leader->IsAlive()) {
            Logger::Debug("[SpawnSystem::UpdateSquadLeaderSpawns] Squad leader %u dead or missing, deactivating spawn %u", loc.squadLeaderId, id);
            loc.isActive = false;
            deactivatedCount++;
            continue;
        }

        // Update spawn position to leader's position
        loc.position = leader->GetPosition();
        bool inCombat = IsSquadLeaderInCombat(loc.squadLeaderId);
        loc.isActive = !inCombat;
        if (inCombat) {
            Logger::Debug("[SpawnSystem::UpdateSquadLeaderSpawns] Squad leader %u in combat, spawn %u inactive", loc.squadLeaderId, id);
        }
        updatedCount++;
    }
    Logger::Debug("[SpawnSystem::UpdateSquadLeaderSpawns] Updated %d squad leader spawns, deactivated %d", updatedCount, deactivatedCount);
    Logger::Trace("[SpawnSystem::UpdateSquadLeaderSpawns] Exit");
}

bool SpawnSystem::CanSpawnOnSquadLeader(uint32_t playerId) const {
    Logger::Trace("[SpawnSystem::CanSpawnOnSquadLeader] Entry, playerId=%u", playerId);
    // Find player's squad leader and check if they can spawn on them
    auto* pm = m_server->GetPlayerManager();
    auto player = pm->GetPlayer(playerId);
    if (!player) {
        Logger::Debug("[SpawnSystem::CanSpawnOnSquadLeader] Player %u not found", playerId);
        Logger::Trace("[SpawnSystem::CanSpawnOnSquadLeader] Exit, return false");
        return false;
    }

    // Check all squad leader spawns for the player's team
    auto* tm = m_server->GetTeamManager();
    uint32_t playerTeam = tm->GetPlayerTeam(playerId);
    Logger::Debug("[SpawnSystem::CanSpawnOnSquadLeader] Checking squad leader spawns for player %u on team %u", playerId, playerTeam);

    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.type != SpawnType::SquadLeader) continue;
        if (loc.teamId != playerTeam) continue;
        if (!loc.isActive) {
            Logger::Trace("[SpawnSystem::CanSpawnOnSquadLeader] Spawn %u inactive, skipping", id);
            continue;
        }

        auto leader = pm->GetPlayer(loc.squadLeaderId);
        if (leader && leader->IsAlive() && !IsSquadLeaderInCombat(loc.squadLeaderId)) {
            Logger::Debug("[SpawnSystem::CanSpawnOnSquadLeader] Found valid squad leader spawn %u (leader=%u)", id, loc.squadLeaderId);
            Logger::Trace("[SpawnSystem::CanSpawnOnSquadLeader] Exit, return true");
            return true;
        }
    }
    Logger::Debug("[SpawnSystem::CanSpawnOnSquadLeader] No valid squad leader spawn found for player %u", playerId);
    Logger::Trace("[SpawnSystem::CanSpawnOnSquadLeader] Exit, return false");
    return false;
}

uint32_t SpawnSystem::CreateTunnel(uint32_t teamId, const Vector3& position, uint32_t objectiveId) {
    Logger::Trace("[SpawnSystem::CreateTunnel] Entry, teamId=%u, position=(%.1f, %.1f, %.1f), objectiveId=%u",
                  teamId, position.x, position.y, position.z, objectiveId);
    SpawnLocation tunnel;
    tunnel.type = SpawnType::Tunnel;
    tunnel.name = "Tunnel";
    tunnel.position = position;
    tunnel.teamId = teamId;
    tunnel.objectiveId = objectiveId;
    tunnel.tunnelHealth = 100;
    tunnel.isActive = true;

    uint32_t id = AddSpawnLocation(tunnel);
    Logger::Info("Tunnel created at (%.1f, %.1f, %.1f) for team %u",
                 position.x, position.y, position.z, teamId);
    Logger::Debug("[SpawnSystem::CreateTunnel] Tunnel id=%u, health=%d, objectiveId=%u", id, tunnel.tunnelHealth, objectiveId);
    Logger::Trace("[SpawnSystem::CreateTunnel] Exit, return id=%u", id);
    return id;
}

void SpawnSystem::DestroyTunnel(uint32_t tunnelId) {
    Logger::Trace("[SpawnSystem::DestroyTunnel] Entry, tunnelId=%u", tunnelId);
    auto* loc = GetSpawnLocation(tunnelId);
    if (loc && loc->type == SpawnType::Tunnel) {
        loc->isDestroyed = true;
        loc->isActive = false;
        Logger::Info("Tunnel %u destroyed", tunnelId);
    } else {
        Logger::Warn("[SpawnSystem::DestroyTunnel] Tunnel %u not found or not a tunnel type", tunnelId);
    }
    Logger::Trace("[SpawnSystem::DestroyTunnel] Exit");
}

bool SpawnSystem::IsTunnelActive(uint32_t tunnelId) const {
    Logger::Trace("[SpawnSystem::IsTunnelActive] Entry, tunnelId=%u", tunnelId);
    auto it = m_spawnLocations.find(tunnelId);
    if (it == m_spawnLocations.end()) {
        Logger::Debug("[SpawnSystem::IsTunnelActive] Tunnel %u not found", tunnelId);
        Logger::Trace("[SpawnSystem::IsTunnelActive] Exit, return false");
        return false;
    }
    bool active = it->second.type == SpawnType::Tunnel &&
                  it->second.isActive && !it->second.isDestroyed;
    Logger::Debug("[SpawnSystem::IsTunnelActive] Tunnel %u: type=%d, isActive=%s, isDestroyed=%s, result=%s",
                  tunnelId, static_cast<int>(it->second.type),
                  it->second.isActive ? "true" : "false",
                  it->second.isDestroyed ? "true" : "false",
                  active ? "true" : "false");
    Logger::Trace("[SpawnSystem::IsTunnelActive] Exit, return %s", active ? "true" : "false");
    return active;
}

void SpawnSystem::StartSpawnWave(uint32_t teamId) {
    Logger::Trace("[SpawnSystem::StartSpawnWave] Entry, teamId=%u", teamId);
    auto& wave = m_waveStates[teamId];
    wave.active = true;
    wave.timer = wave.interval;
    Logger::Info("[SpawnSystem::StartSpawnWave] Spawn wave started for team %u, interval=%.1fs", teamId, wave.interval);
    Logger::Trace("[SpawnSystem::StartSpawnWave] Exit");
}

bool SpawnSystem::IsInSpawnWave(uint32_t teamId) const {
    Logger::Trace("[SpawnSystem::IsInSpawnWave] Entry, teamId=%u", teamId);
    auto it = m_waveStates.find(teamId);
    bool result = it != m_waveStates.end() && it->second.active;
    Logger::Debug("[SpawnSystem::IsInSpawnWave] Team %u spawn wave active: %s", teamId, result ? "true" : "false");
    Logger::Trace("[SpawnSystem::IsInSpawnWave] Exit, return %s", result ? "true" : "false");
    return result;
}

float SpawnSystem::GetWaveTimeRemaining(uint32_t teamId) const {
    Logger::Trace("[SpawnSystem::GetWaveTimeRemaining] Entry, teamId=%u", teamId);
    auto it = m_waveStates.find(teamId);
    float remaining = it != m_waveStates.end() ? it->second.timer : 0.0f;
    Logger::Debug("[SpawnSystem::GetWaveTimeRemaining] Team %u wave time remaining: %.1fs", teamId, remaining);
    Logger::Trace("[SpawnSystem::GetWaveTimeRemaining] Exit, return %.1f", remaining);
    return remaining;
}

bool SpawnSystem::SpawnPlayer(uint32_t playerId, uint32_t spawnLocationId) {
    Logger::Trace("[SpawnSystem::SpawnPlayer] Entry, playerId=%u, spawnLocationId=%u", playerId, spawnLocationId);
    auto* loc = GetSpawnLocation(spawnLocationId);
    if (!loc || !loc->isActive || loc->isDestroyed) {
        Logger::Warn("Cannot spawn player %u at location %u (invalid/inactive)", playerId, spawnLocationId);
        Logger::Trace("[SpawnSystem::SpawnPlayer] Exit, return false (invalid location)");
        return false;
    }

    auto* pm = m_server->GetPlayerManager();
    auto player = pm->GetPlayer(playerId);
    if (!player) {
        Logger::Warn("[SpawnSystem::SpawnPlayer] Player %u not found", playerId);
        Logger::Trace("[SpawnSystem::SpawnPlayer] Exit, return false (player not found)");
        return false;
    }

    Vector3 spawnPos = loc->position + GetSpawnOffset(*loc);
    player->SetPosition(spawnPos);
    player->SetOrientation(loc->rotation);
    pm->OnPlayerSpawn(playerId);
    Logger::Debug("[SpawnSystem::SpawnPlayer] Player %u position set to (%.1f, %.1f, %.1f)", playerId, spawnPos.x, spawnPos.y, spawnPos.z);

    // Apply spawn cooldown to prevent spawn-camping
    if (loc->type == SpawnType::SquadLeader) {
        loc->spawnCooldown = m_squadSpawnCooldown;
        Logger::Debug("[SpawnSystem::SpawnPlayer] Applied squad spawn cooldown: %.1fs", m_squadSpawnCooldown);
    } else if (loc->type == SpawnType::Tunnel) {
        loc->spawnCooldown = m_tunnelSpawnCooldown;
        Logger::Debug("[SpawnSystem::SpawnPlayer] Applied tunnel spawn cooldown: %.1fs", m_tunnelSpawnCooldown);
    } else {
        Logger::Debug("[SpawnSystem::SpawnPlayer] No cooldown applied for spawn type %d", static_cast<int>(loc->type));
    }

    Logger::Debug("Player %u spawned at '%s' (%.1f, %.1f, %.1f)",
                  playerId, loc->name.c_str(), spawnPos.x, spawnPos.y, spawnPos.z);
    Logger::Trace("[SpawnSystem::SpawnPlayer] Exit, return true");
    return true;
}

bool SpawnSystem::SpawnPlayerAtDefault(uint32_t playerId) {
    Logger::Trace("[SpawnSystem::SpawnPlayerAtDefault] Entry, playerId=%u", playerId);
    auto spawns = GetAvailableSpawns(playerId);
    Logger::Debug("[SpawnSystem::SpawnPlayerAtDefault] Found %zu available spawns for player %u", spawns.size(), playerId);

    // Prefer base spawn
    for (const auto* sp : spawns) {
        if (sp->type == SpawnType::BaseSpawn) {
            Logger::Debug("[SpawnSystem::SpawnPlayerAtDefault] Found base spawn '%s' (id=%u)", sp->name.c_str(), sp->id);
            bool result = SpawnPlayer(playerId, sp->id);
            Logger::Trace("[SpawnSystem::SpawnPlayerAtDefault] Exit, return %s (base spawn)", result ? "true" : "false");
            return result;
        }
    }
    Logger::Debug("[SpawnSystem::SpawnPlayerAtDefault] No base spawn found, looking for fallback");

    // Fall back to any available
    if (!spawns.empty()) {
        Logger::Debug("[SpawnSystem::SpawnPlayerAtDefault] Falling back to spawn '%s' (id=%u)", spawns.front()->name.c_str(), spawns.front()->id);
        bool result = SpawnPlayer(playerId, spawns.front()->id);
        Logger::Trace("[SpawnSystem::SpawnPlayerAtDefault] Exit, return %s (fallback spawn)", result ? "true" : "false");
        return result;
    }
    Logger::Warn("[SpawnSystem::SpawnPlayerAtDefault] No available spawn locations for player %u", playerId);
    Logger::Trace("[SpawnSystem::SpawnPlayerAtDefault] Exit, return false (no spawns available)");
    return false;
}

void SpawnSystem::Update(float deltaSeconds) {
    Logger::Trace("[SpawnSystem::Update] Entry, deltaSeconds=%.4f", deltaSeconds);

    // Update squad leader spawns
    UpdateSquadLeaderSpawns();

    // Update spawn cooldowns
    int cooldownsUpdated = 0;
    for (auto& [id, loc] : m_spawnLocations) {
        if (loc.spawnCooldown > 0.0f) {
            loc.spawnCooldown -= deltaSeconds;
            if (loc.spawnCooldown < 0.0f) loc.spawnCooldown = 0.0f;
            cooldownsUpdated++;
        }
    }
    if (cooldownsUpdated > 0) {
        Logger::Debug("[SpawnSystem::Update] Updated %d spawn cooldowns", cooldownsUpdated);
    }

    // Update wave spawns
    for (auto& [teamId, wave] : m_waveStates) {
        if (!wave.active) {
            Logger::Trace("[SpawnSystem::Update] Team %u wave inactive, skipping", teamId);
            continue;
        }
        wave.timer -= deltaSeconds;
        if (wave.timer <= 0.0f) {
            Logger::Info("[SpawnSystem::Update] Spawn wave triggered for team %u", teamId);
            // Spawn all pending players
            auto* pm = m_server->GetPlayerManager();
            int spawnedCount = 0;
            for (auto& player : pm->GetDeadPlayers()) {
                uint32_t pid = player->GetConnection()->GetClientId();
                auto* tm = m_server->GetTeamManager();
                if (tm->GetPlayerTeam(pid) == teamId) {
                    Logger::Debug("[SpawnSystem::Update] Spawning dead player %u in wave for team %u", pid, teamId);
                    SpawnPlayerAtDefault(pid);
                    spawnedCount++;
                }
            }
            wave.timer = wave.interval;
            Logger::Debug("Spawn wave for team %u", teamId);
            Logger::Debug("[SpawnSystem::Update] Wave complete: spawned %d players, next wave in %.1fs", spawnedCount, wave.interval);
        }
    }
    Logger::Trace("[SpawnSystem::Update] Exit");
}

void SpawnSystem::SetWaveInterval(float seconds) {
    Logger::Trace("[SpawnSystem::SetWaveInterval] Entry, seconds=%.1f", seconds);
    m_waveInterval = seconds;
    for (auto& [teamId, wave] : m_waveStates) {
        wave.interval = seconds;
        Logger::Debug("[SpawnSystem::SetWaveInterval] Team %u wave interval updated to %.1fs", teamId, seconds);
    }
    Logger::Info("[SpawnSystem::SetWaveInterval] Wave interval set to %.1fs for all teams", seconds);
    Logger::Trace("[SpawnSystem::SetWaveInterval] Exit");
}

void SpawnSystem::SetSquadSpawnCooldown(float seconds) {
    Logger::Trace("[SpawnSystem::SetSquadSpawnCooldown] Entry, seconds=%.1f", seconds);
    m_squadSpawnCooldown = seconds;
    Logger::Info("[SpawnSystem::SetSquadSpawnCooldown] Squad spawn cooldown set to %.1fs", seconds);
    Logger::Trace("[SpawnSystem::SetSquadSpawnCooldown] Exit");
}

void SpawnSystem::SetTunnelSpawnCooldown(float seconds) {
    Logger::Trace("[SpawnSystem::SetTunnelSpawnCooldown] Entry, seconds=%.1f", seconds);
    m_tunnelSpawnCooldown = seconds;
    Logger::Info("[SpawnSystem::SetTunnelSpawnCooldown] Tunnel spawn cooldown set to %.1fs", seconds);
    Logger::Trace("[SpawnSystem::SetTunnelSpawnCooldown] Exit");
}

bool SpawnSystem::IsSquadLeaderInCombat(uint32_t leaderId) const {
    Logger::Trace("[SpawnSystem::IsSquadLeaderInCombat] Entry, leaderId=%u", leaderId);
    // A squad leader is "in combat" if they took damage recently
    // For now, approximate by checking health < max
    auto* pm = m_server->GetPlayerManager();
    auto leader = pm->GetPlayer(leaderId);
    if (!leader) {
        Logger::Debug("[SpawnSystem::IsSquadLeaderInCombat] Leader %u not found, assuming in combat", leaderId);
        Logger::Trace("[SpawnSystem::IsSquadLeaderInCombat] Exit, return true (leader not found)");
        return true;  // Err on the side of caution
    }
    int health = leader->GetHealth();
    bool inCombat = health < 80;  // Recently damaged
    Logger::Debug("[SpawnSystem::IsSquadLeaderInCombat] Leader %u health=%d, inCombat=%s", leaderId, health, inCombat ? "true" : "false");
    Logger::Trace("[SpawnSystem::IsSquadLeaderInCombat] Exit, return %s", inCombat ? "true" : "false");
    return inCombat;
}

Vector3 SpawnSystem::GetSpawnOffset(const SpawnLocation& loc) const {
    Logger::Trace("[SpawnSystem::GetSpawnOffset] Entry, spawnId=%u", loc.id);
    // Add small random offset to prevent overlapping spawns
    float ox = (std::rand() % 10 - 5) * 0.5f;
    float oy = (std::rand() % 10 - 5) * 0.5f;
    Logger::Debug("[SpawnSystem::GetSpawnOffset] Random offset: (%.1f, %.1f, 0.0)", ox, oy);
    Logger::Trace("[SpawnSystem::GetSpawnOffset] Exit, return offset=(%.1f, %.1f, 0.0)", ox, oy);
    return Vector3(ox, oy, 0.0f);
}
