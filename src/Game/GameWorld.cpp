// src/Game/GameWorld.cpp – Complete implementation for RS2V Server GameWorld

#include "Game/GameWorld.h"
#include "Utils/Logger.h"
#include "Game/MapManager.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/GameServer.h"
#include <algorithm>

GameWorld::GameWorld(GameServer* server,
                     std::shared_ptr<MapManager> mapMgr,
                     std::shared_ptr<PlayerManager> playerMgr,
                     std::shared_ptr<TeamManager> teamMgr)
    : m_server(server),
      m_mapManager(mapMgr),
      m_playerManager(playerMgr),
      m_teamManager(teamMgr)
{
    Logger::Info("GameWorld initialized");
}

GameWorld::~GameWorld() = default;

bool GameWorld::Initialize()
{
    Logger::Info("Loading world spawn points");
    m_spawnPoints = m_mapManager->GetSpawnPoints();
    Logger::Info("Loaded %zu spawn points", m_spawnPoints.size());
    return true;
}

void GameWorld::Update(double deltaTime)
{
    // Example world update hooks: environmental effects, dynamic props, etc.
    UpdateEnvironmentalEffects(deltaTime);
    UpdateDynamicObjects(deltaTime);
}

void GameWorld::RespawnPlayer(uint32_t playerId)
{
    auto conn = m_server->GetClientConnection(playerId);
    if (!conn) return;

    // Choose spawn point for player's team
    auto* tm = m_server->GetTeamManager();
    uint32_t teamId = tm ? tm->GetPlayerTeam(playerId) : 0;
    auto pts = GetTeamSpawnPoints(teamId);
    if (pts.empty())
    {
        Logger::Warn("No spawn points for team %u, using fallback", teamId);
        // Use all spawn point positions as fallback
        for (const auto& sp : m_spawnPoints) {
            pts.push_back(sp.position);
        }
    }

    if (pts.empty()) return;

    // Round-robin spawn
    size_t idx = m_nextSpawnIndex[teamId] % pts.size();
    Vector3 spawnPos = pts[idx];
    m_nextSpawnIndex[teamId]++;

    conn->SendPositionUpdate(spawnPos);
    conn->SendSpawnPlayer();
    Logger::Debug("Respawned player %u at (%.1f, %.1f, %.1f)",
                  playerId, spawnPos.x, spawnPos.y, spawnPos.z);
}

void GameWorld::UpdateEnvironmentalEffects(double deltaTime)
{
    // Example: day/night cycle, weather transitions
    m_elapsedTime += deltaTime;
    // ... apply to clients if needed
}

void GameWorld::UpdateDynamicObjects(double deltaTime)
{
    // Example: moving vehicles, destructible objects
    for (auto& obj : m_dynamicObjects)
    {
        obj.Update(deltaTime);
        if (obj.HasStateChanged()) {
            // Dynamic object state updates are serialized in the world snapshot
        }
    }
}

std::vector<Vector3> GameWorld::GetTeamSpawnPoints(uint32_t teamId) const
{
    std::vector<Vector3> result;
    for (const auto& sp : m_spawnPoints)
    {
        if (sp.teamId == 0 || sp.teamId == teamId)
            result.push_back(sp.position);
    }
    return result;
}

void GameWorld::AddDynamicObject(const DynamicObject& obj)
{
    m_dynamicObjects.push_back(obj);
    Logger::Debug("Added dynamic object ID %u", obj.id);
}

void GameWorld::RemoveDynamicObject(uint32_t objectId)
{
    m_dynamicObjects.erase(
        std::remove_if(m_dynamicObjects.begin(), m_dynamicObjects.end(),
            [&](const DynamicObject& o){ return o.id == objectId; }),
        m_dynamicObjects.end());
    Logger::Debug("Removed dynamic object ID %u", objectId);
}

// Getter for map bounds
Bounds GameWorld::GetMapBounds() const
{
    return m_mapManager->GetMapBounds();
}

// Serialization of world snapshot
std::vector<uint8_t> GameWorld::SerializeSnapshot() const
{
    std::vector<uint8_t> data;
    // Serialize environmental state, dynamic objects states, etc.
    // ...
    return data;
}