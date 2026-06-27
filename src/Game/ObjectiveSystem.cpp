// src/Game/ObjectiveSystem.cpp
// RS2V objective/capture zone system implementation

#include "Game/ObjectiveSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Network/NetworkManager.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cstring>

ObjectiveSystem::ObjectiveSystem(GameServer* server)
    : m_server(server)
{
}

ObjectiveSystem::~ObjectiveSystem() {
    Shutdown();
}

void ObjectiveSystem::Initialize() {
    m_objectives.clear();
    m_territoryOrder.clear();
    m_currentTerritoryIndex = 0;
    Logger::Info("ObjectiveSystem initialized");
}

void ObjectiveSystem::Shutdown() {
    m_objectives.clear();
}

uint32_t ObjectiveSystem::AddObjective(const CaptureZone& zone) {
    CaptureZone z = zone;
    // Preserve a caller-supplied id (e.g. one loaded from map data) so the
    // ObjectiveSystem, MapManager and GameState all agree on objective ids.
    // Fall back to an auto-assigned id when none was provided (id == 0).
    if (z.id == 0) {
        z.id = m_nextObjectiveId++;
    } else if (z.id >= m_nextObjectiveId) {
        m_nextObjectiveId = z.id + 1;
    }
    m_objectives[z.id] = z;
    Logger::Info("Objective added: '%s' (id=%u) at (%.1f, %.1f, %.1f) radius=%.1f",
                 z.name.c_str(), z.id, z.position.x, z.position.y, z.position.z, z.captureRadius);
    return z.id;
}

void ObjectiveSystem::Clear() {
    m_objectives.clear();
    m_territoryOrder.clear();
    m_currentTerritoryIndex = 0;
    m_nextObjectiveId = 1;
    Logger::Debug("ObjectiveSystem cleared");
}

void ObjectiveSystem::RemoveObjective(uint32_t objectiveId) {
    m_objectives.erase(objectiveId);
}

CaptureZone* ObjectiveSystem::GetObjective(uint32_t id) {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() ? &it->second : nullptr;
}

const CaptureZone* ObjectiveSystem::GetObjective(uint32_t id) const {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() ? &it->second : nullptr;
}

std::vector<const CaptureZone*> ObjectiveSystem::GetAllObjectives() const {
    std::vector<const CaptureZone*> result;
    for (const auto& [id, zone] : m_objectives) {
        result.push_back(&zone);
    }
    return result;
}

std::vector<const CaptureZone*> ObjectiveSystem::GetActiveObjectives() const {
    std::vector<const CaptureZone*> result;
    for (const auto& [id, zone] : m_objectives) {
        if (zone.isActive) result.push_back(&zone);
    }
    return result;
}

void ObjectiveSystem::SetTerritoryOrder(const std::vector<uint32_t>& objectiveIds) {
    m_territoryOrder = objectiveIds;
    m_currentTerritoryIndex = 0;

    // Lock all objectives except the first
    for (auto& [id, zone] : m_objectives) {
        zone.isActive = false;
        zone.state = CaptureState::Locked;
    }

    if (!m_territoryOrder.empty()) {
        auto* first = GetObjective(m_territoryOrder[0]);
        if (first) {
            first->isActive = true;
            first->state = CaptureState::Neutral;
            Logger::Info("Territory mode: first objective '%s' activated", first->name.c_str());
        }
    }
}

void ObjectiveSystem::ActivateNextTerritory(uint32_t capturingTeamDirection) {
    // Lock current objective
    if (m_currentTerritoryIndex < (int)m_territoryOrder.size()) {
        auto* current = GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
        if (current) {
            current->isActive = false;
            current->state = CaptureState::Locked;
        }
    }

    // Advance to next
    m_currentTerritoryIndex++;
    if (m_currentTerritoryIndex < (int)m_territoryOrder.size()) {
        auto* next = GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
        if (next) {
            next->isActive = true;
            next->state = CaptureState::Neutral;
            next->captureProgress = 0.0f;
            Logger::Info("Territory mode: next objective '%s' activated (index %d)",
                         next->name.c_str(), m_currentTerritoryIndex);
        }
    } else {
        Logger::Info("Territory mode: all objectives captured by advancing team");
    }
}

const CaptureZone* ObjectiveSystem::GetCurrentTerritoryObjective() const {
    if (m_currentTerritoryIndex < (int)m_territoryOrder.size()) {
        return GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
    }
    return nullptr;
}

void ObjectiveSystem::Update(float deltaSeconds) {
    RefreshPlayerZones();

    for (auto& [id, zone] : m_objectives) {
        if (!zone.isActive) continue;
        ProcessCapture(zone, deltaSeconds);
    }
}

void ObjectiveSystem::RefreshPlayerZones() {
    // Clear existing zone occupants
    for (auto& [id, zone] : m_objectives) {
        zone.attackerIds.clear();
        zone.defenderIds.clear();
    }

    auto* pm = m_server->GetPlayerManager();
    auto* tm = m_server->GetTeamManager();
    if (!pm || !tm) return;

    for (auto& player : pm->GetAlivePlayers()) {
        uint32_t pid = player->GetConnection()->GetClientId();
        uint32_t playerTeam = tm->GetPlayerTeam(pid);
        Vector3 playerPos = player->GetPosition();

        for (auto& [id, zone] : m_objectives) {
            if (!zone.isActive) continue;
            if (!IsPlayerInZone(pid, zone)) continue;

            if (zone.controllingTeam == 0 || zone.controllingTeam != playerTeam) {
                zone.attackerIds.push_back(pid);
            } else {
                zone.defenderIds.push_back(pid);
            }
        }
    }
}

void ObjectiveSystem::ProcessCapture(CaptureZone& zone, float deltaSeconds) {
    size_t attackers = zone.attackerIds.size();
    size_t defenders = zone.defenderIds.size();

    if (attackers == 0 && defenders == 0) {
        // Nobody present: slow decay toward neutral
        if (zone.captureProgress > 0.0f) {
            zone.captureProgress -= zone.decaySpeed * deltaSeconds;
            if (zone.captureProgress <= 0.0f) {
                zone.captureProgress = 0.0f;
                zone.state = CaptureState::Neutral;
            }
        }
        return;
    }

    if (attackers > 0 && defenders > 0) {
        // Contested: no capture progress, slow decay
        zone.state = CaptureState::Contested;
        zone.captureProgress -= zone.contestDecaySpeed * deltaSeconds;
        if (zone.captureProgress < 0.0f) zone.captureProgress = 0.0f;
        return;
    }

    if (attackers > 0 && defenders == 0) {
        // Attackers only: advance capture
        zone.state = CaptureState::Capturing;

        // More attackers = faster capture (diminishing returns)
        float speedMultiplier = 1.0f + 0.5f * (std::min(attackers, (size_t)5) - 1);
        zone.captureProgress += zone.captureSpeed * speedMultiplier * deltaSeconds;

        if (zone.captureProgress >= 1.0f) {
            zone.captureProgress = 1.0f;

            // Determine which team captured it. attackerIds is guaranteed non-empty
            // here (attackers > 0), but GetTeamManager() may be null during teardown;
            // guard the dereference rather than trust it blindly.
            auto* tm = m_server ? m_server->GetTeamManager() : nullptr;
            if (!tm) {
                Logger::Warn("[ObjectiveSystem::ProcessCapture] No TeamManager available; skipping capture resolution");
                return;
            }
            uint32_t capturingTeam = tm->GetPlayerTeam(zone.attackerIds.front());
            OnObjectiveCaptured(zone, capturingTeam);
        }
    }

    if (defenders > 0 && attackers == 0) {
        // Defenders only: slow regain / reinforce
        zone.state = CaptureState::Controlled;
        if (zone.captureProgress > 0.0f) {
            zone.captureProgress -= zone.decaySpeed * 2.0f * deltaSeconds;
            if (zone.captureProgress < 0.0f) zone.captureProgress = 0.0f;
        }
    }
}

void ObjectiveSystem::OnObjectiveCaptured(CaptureZone& zone, uint32_t newTeam) {
    uint32_t previousTeam = zone.controllingTeam;
    zone.controllingTeam = newTeam;
    zone.state = CaptureState::Controlled;
    zone.captureProgress = 0.0f;
    zone.attackerIds.clear();

    Logger::Info("Objective '%s' captured by team %u (was team %u)",
                 zone.name.c_str(), newTeam, previousTeam);

    if (m_capturedCallback) {
        m_capturedCallback(zone.id, newTeam, previousTeam);
    }

    // In Territory mode, activate next objective
    if (zone.type == ObjectiveType::Territory) {
        ActivateNextTerritory(newTeam);
    }

    BroadcastObjectiveStates();
}

bool ObjectiveSystem::IsPlayerInZone(uint32_t playerId, const CaptureZone& zone) const {
    auto* pm = m_server->GetPlayerManager();
    auto player = pm->GetPlayer(playerId);
    if (!player) return false;

    Vector3 playerPos = player->GetPosition();
    float dx = playerPos.x - zone.position.x;
    float dy = playerPos.y - zone.position.y;
    // 2D distance check (ignore Z for capture zones)
    float distSq = dx * dx + dy * dy;
    return distSq <= zone.captureRadius * zone.captureRadius;
}

void ObjectiveSystem::OnPlayerEnterZone(uint32_t playerId, uint32_t objectiveId) {
    // Handled by RefreshPlayerZones in Update
}

void ObjectiveSystem::OnPlayerLeaveZone(uint32_t playerId, uint32_t objectiveId) {
    // Handled by RefreshPlayerZones in Update
}

uint32_t ObjectiveSystem::GetObjectiveCount() const {
    return static_cast<uint32_t>(m_objectives.size());
}

uint32_t ObjectiveSystem::GetTeamObjectiveCount(uint32_t teamId) const {
    uint32_t count = 0;
    for (const auto& [id, zone] : m_objectives) {
        if (zone.controllingTeam == teamId) count++;
    }
    return count;
}

bool ObjectiveSystem::AreAllObjectivesCapturedBy(uint32_t teamId) const {
    for (const auto& [id, zone] : m_objectives) {
        if (zone.controllingTeam != teamId) return false;
    }
    return true;
}

void ObjectiveSystem::SetOnObjectiveCaptured(ObjectiveCapturedCallback cb) {
    m_capturedCallback = std::move(cb);
}

void ObjectiveSystem::BroadcastObjectiveStates() const {
    std::vector<uint8_t> data;
    uint32_t count = static_cast<uint32_t>(m_objectives.size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&count),
                reinterpret_cast<uint8_t*>(&count) + sizeof(count));

    for (const auto& [id, zone] : m_objectives) {
        // id, controllingTeam, state, captureProgress, isActive
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.id),
                    reinterpret_cast<const uint8_t*>(&zone.id) + sizeof(zone.id));
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.controllingTeam),
                    reinterpret_cast<const uint8_t*>(&zone.controllingTeam) + sizeof(zone.controllingTeam));
        uint8_t stateVal = static_cast<uint8_t>(zone.state);
        data.push_back(stateVal);
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.captureProgress),
                    reinterpret_cast<const uint8_t*>(&zone.captureProgress) + sizeof(zone.captureProgress));
        data.push_back(zone.isActive ? 1 : 0);
    }

    m_server->GetNetworkManager()->BroadcastPacket("OBJECTIVE_UPDATE", data);
}
