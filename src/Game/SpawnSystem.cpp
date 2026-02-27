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
}

SpawnSystem::~SpawnSystem() {
    Shutdown();
}

void SpawnSystem::Initialize() {
    m_spawnLocations.clear();
    m_waveStates.clear();
    Logger::Info("SpawnSystem initialized");
}

void SpawnSystem::Shutdown() {
    m_spawnLocations.clear();
    m_waveStates.clear();
}

uint32_t SpawnSystem::AddSpawnLocation(const SpawnLocation& loc) {
    SpawnLocation l = loc;
    l.id = m_nextSpawnId++;
    m_spawnLocations[l.id] = l;
    Logger::Info("Spawn location added: '%s' (id=%u, type=%d, team=%u) at (%.1f, %.1f, %.1f)",
                 l.name.c_str(), l.id, static_cast<int>(l.type), l.teamId,
                 l.position.x, l.position.y, l.position.z);
    return l.id;
}

void SpawnSystem::RemoveSpawnLocation(uint32_t id) {
    m_spawnLocations.erase(id);
}

SpawnLocation* SpawnSystem::GetSpawnLocation(uint32_t id) {
    auto it = m_spawnLocations.find(id);
    return it != m_spawnLocations.end() ? &it->second : nullptr;
}

std::vector<const SpawnLocation*> SpawnSystem::GetAvailableSpawns(uint32_t playerId) const {
    std::vector<const SpawnLocation*> result;
    auto* tm = m_server->GetTeamManager();
    uint32_t playerTeam = tm->GetPlayerTeam(playerId);

    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.teamId != playerTeam) continue;
        if (!loc.isActive || loc.isDestroyed) continue;
        if (loc.spawnCooldown > 0.0f) continue;

        // Squad leader spawns require special checks
        if (loc.type == SpawnType::SquadLeader) {
            if (!CanSpawnOnSquadLeader(playerId)) continue;
        }

        result.push_back(&loc);
    }
    return result;
}

std::vector<const SpawnLocation*> SpawnSystem::GetTeamSpawns(uint32_t teamId) const {
    std::vector<const SpawnLocation*> result;
    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.teamId == teamId) result.push_back(&loc);
    }
    return result;
}

void SpawnSystem::UpdateSquadLeaderSpawns() {
    // Remove stale squad leader spawns and update positions
    std::vector<uint32_t> toRemove;
    auto* pm = m_server->GetPlayerManager();

    for (auto& [id, loc] : m_spawnLocations) {
        if (loc.type != SpawnType::SquadLeader) continue;

        auto leader = pm->GetPlayer(loc.squadLeaderId);
        if (!leader || !leader->IsAlive()) {
            loc.isActive = false;
            continue;
        }

        // Update spawn position to leader's position
        loc.position = leader->GetPosition();
        loc.isActive = !IsSquadLeaderInCombat(loc.squadLeaderId);
    }
}

bool SpawnSystem::CanSpawnOnSquadLeader(uint32_t playerId) const {
    // Find player's squad leader and check if they can spawn on them
    auto* pm = m_server->GetPlayerManager();
    auto player = pm->GetPlayer(playerId);
    if (!player) return false;

    // Check all squad leader spawns for the player's team
    auto* tm = m_server->GetTeamManager();
    uint32_t playerTeam = tm->GetPlayerTeam(playerId);

    for (const auto& [id, loc] : m_spawnLocations) {
        if (loc.type != SpawnType::SquadLeader) continue;
        if (loc.teamId != playerTeam) continue;
        if (!loc.isActive) continue;

        auto leader = pm->GetPlayer(loc.squadLeaderId);
        if (leader && leader->IsAlive() && !IsSquadLeaderInCombat(loc.squadLeaderId)) {
            return true;
        }
    }
    return false;
}

uint32_t SpawnSystem::CreateTunnel(uint32_t teamId, const Vector3& position, uint32_t objectiveId) {
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
    return id;
}

void SpawnSystem::DestroyTunnel(uint32_t tunnelId) {
    auto* loc = GetSpawnLocation(tunnelId);
    if (loc && loc->type == SpawnType::Tunnel) {
        loc->isDestroyed = true;
        loc->isActive = false;
        Logger::Info("Tunnel %u destroyed", tunnelId);
    }
}

bool SpawnSystem::IsTunnelActive(uint32_t tunnelId) const {
    auto it = m_spawnLocations.find(tunnelId);
    if (it == m_spawnLocations.end()) return false;
    return it->second.type == SpawnType::Tunnel &&
           it->second.isActive && !it->second.isDestroyed;
}

void SpawnSystem::StartSpawnWave(uint32_t teamId) {
    auto& wave = m_waveStates[teamId];
    wave.active = true;
    wave.timer = wave.interval;
}

bool SpawnSystem::IsInSpawnWave(uint32_t teamId) const {
    auto it = m_waveStates.find(teamId);
    return it != m_waveStates.end() && it->second.active;
}

float SpawnSystem::GetWaveTimeRemaining(uint32_t teamId) const {
    auto it = m_waveStates.find(teamId);
    return it != m_waveStates.end() ? it->second.timer : 0.0f;
}

bool SpawnSystem::SpawnPlayer(uint32_t playerId, uint32_t spawnLocationId) {
    auto* loc = GetSpawnLocation(spawnLocationId);
    if (!loc || !loc->isActive || loc->isDestroyed) {
        Logger::Warn("Cannot spawn player %u at location %u (invalid/inactive)", playerId, spawnLocationId);
        return false;
    }

    auto* pm = m_server->GetPlayerManager();
    auto player = pm->GetPlayer(playerId);
    if (!player) return false;

    Vector3 spawnPos = loc->position + GetSpawnOffset(*loc);
    player->SetPosition(spawnPos);
    player->SetOrientation(loc->rotation);
    pm->OnPlayerSpawn(playerId);

    // Apply spawn cooldown to prevent spawn-camping
    if (loc->type == SpawnType::SquadLeader) {
        loc->spawnCooldown = m_squadSpawnCooldown;
    } else if (loc->type == SpawnType::Tunnel) {
        loc->spawnCooldown = m_tunnelSpawnCooldown;
    }

    Logger::Debug("Player %u spawned at '%s' (%.1f, %.1f, %.1f)",
                  playerId, loc->name.c_str(), spawnPos.x, spawnPos.y, spawnPos.z);
    return true;
}

bool SpawnSystem::SpawnPlayerAtDefault(uint32_t playerId) {
    auto spawns = GetAvailableSpawns(playerId);
    // Prefer base spawn
    for (const auto* sp : spawns) {
        if (sp->type == SpawnType::BaseSpawn) {
            return SpawnPlayer(playerId, sp->id);
        }
    }
    // Fall back to any available
    if (!spawns.empty()) {
        return SpawnPlayer(playerId, spawns.front()->id);
    }
    return false;
}

void SpawnSystem::Update(float deltaSeconds) {
    // Update squad leader spawns
    UpdateSquadLeaderSpawns();

    // Update spawn cooldowns
    for (auto& [id, loc] : m_spawnLocations) {
        if (loc.spawnCooldown > 0.0f) {
            loc.spawnCooldown -= deltaSeconds;
            if (loc.spawnCooldown < 0.0f) loc.spawnCooldown = 0.0f;
        }
    }

    // Update wave spawns
    for (auto& [teamId, wave] : m_waveStates) {
        if (!wave.active) continue;
        wave.timer -= deltaSeconds;
        if (wave.timer <= 0.0f) {
            // Spawn all pending players
            auto* pm = m_server->GetPlayerManager();
            for (auto& player : pm->GetDeadPlayers()) {
                uint32_t pid = player->GetConnection()->GetClientId();
                auto* tm = m_server->GetTeamManager();
                if (tm->GetPlayerTeam(pid) == teamId) {
                    SpawnPlayerAtDefault(pid);
                }
            }
            wave.timer = wave.interval;
            Logger::Debug("Spawn wave for team %u", teamId);
        }
    }
}

void SpawnSystem::SetWaveInterval(float seconds) {
    m_waveInterval = seconds;
    for (auto& [teamId, wave] : m_waveStates) {
        wave.interval = seconds;
    }
}

void SpawnSystem::SetSquadSpawnCooldown(float seconds) {
    m_squadSpawnCooldown = seconds;
}

void SpawnSystem::SetTunnelSpawnCooldown(float seconds) {
    m_tunnelSpawnCooldown = seconds;
}

bool SpawnSystem::IsSquadLeaderInCombat(uint32_t leaderId) const {
    // A squad leader is "in combat" if they took damage recently
    // For now, approximate by checking health < max
    auto* pm = m_server->GetPlayerManager();
    auto leader = pm->GetPlayer(leaderId);
    if (!leader) return true;  // Err on the side of caution
    return leader->GetHealth() < 80;  // Recently damaged
}

Vector3 SpawnSystem::GetSpawnOffset(const SpawnLocation& loc) const {
    // Add small random offset to prevent overlapping spawns
    float ox = (std::rand() % 10 - 5) * 0.5f;
    float oy = (std::rand() % 10 - 5) * 0.5f;
    return Vector3(ox, oy, 0.0f);
}
