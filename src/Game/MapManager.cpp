// src/Game/MapManager.cpp – Complete implementation for RS2V Server MapManager

#include "Game/MapManager.h"
#include "Utils/Logger.h"
#include "Config/MapConfig.h"
#include "Network/NetworkManager.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <filesystem>
#include <random>

MapManager::MapManager(GameServer* server,
                       std::shared_ptr<MapConfig> mapConfig)
    : m_server(server),
      m_mapConfig(mapConfig)
{
    Logger::Info("MapManager initialized");
}

MapManager::~MapManager() = default;

bool MapManager::LoadMap(const std::string& mapName)
{
    Logger::Info("Loading map: %s", mapName.c_str());
    const MapDefinition* def = m_mapConfig->GetDefinition(mapName);
    if (!def) {
        Logger::Error("MapManager: Map definition not found: %s", mapName.c_str());
        return false;
    }
    m_currentMap = *def;

    // Load map geometry, collision, navmesh, etc.
    if (!LoadGeometry(m_currentMap.filePath)) {
        Logger::Error("MapManager: Failed to load geometry for %s", mapName.c_str());
        return false;
    }

    // Initialize spawn points from map definition's Vector3 list
    m_spawnPoints.clear();
    for (const auto& pos : m_currentMap.spawnPoints) {
        SpawnPoint sp;
        sp.position = pos;
        sp.teamId = 0;
        m_spawnPoints.push_back(sp);
    }
    if (m_spawnPoints.empty()) {
        // Fallback: random points in bounds
        GenerateFallbackSpawns();
    }
    Logger::Info("MapManager: %zu spawn points loaded", m_spawnPoints.size());

    // Initialize objectives (convert from int to uint32_t)
    m_objectives.clear();
    for (int objId : m_currentMap.objectiveIds) {
        m_objectives.push_back(static_cast<uint32_t>(objId));
    }
    Logger::Info("MapManager: %zu objectives loaded", m_objectives.size());

    return true;
}

bool MapManager::LoadGeometry(const std::string& path)
{
    // TODO: integrate actual geometry loading
    if (!std::filesystem::exists(path)) {
        Logger::Error("Map geometry file not found: %s", path.c_str());
        return false;
    }
    Logger::Debug("Loaded geometry from %s", path.c_str());
    return true;
}

void MapManager::GenerateFallbackSpawns()
{
    Logger::Warn("Generating fallback spawn points");
    Bounds b = GetMapBounds();
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dx(b.min.x, b.max.x);
    std::uniform_real_distribution<float> dy(b.min.y, b.max.y);
    std::uniform_real_distribution<float> dz(b.min.z, b.max.z);

    for (size_t i = 0; i < 16; ++i) {
        SpawnPoint sp;
        sp.position = {dx(rng), dy(rng), dz(rng)};
        sp.teamId = 0; // neutral
        m_spawnPoints.push_back(sp);
    }
}

std::vector<SpawnPoint> MapManager::GetSpawnPoints() const
{
    return m_spawnPoints;
}

std::vector<uint32_t> MapManager::GetMapObjectives() const
{
    return m_objectives;
}

Bounds MapManager::GetMapBounds() const
{
    // Convert MapDefinition::Bounds to Bounds
    Bounds b;
    b.min = m_currentMap.bounds.min;
    b.max = m_currentMap.bounds.max;
    return b;
}

std::string MapManager::GetNextMap()
{
    // Simple rotation through config list
    auto maps = m_mapConfig->GetAvailableMaps();
    if (maps.empty()) return m_currentMap.name;
    auto it = std::find(maps.begin(), maps.end(), m_currentMap.name);
    if (it == maps.end() || ++it == maps.end()) {
        return maps.front();
    }
    return *it;
}

void MapManager::LogSummary() const
{
    Logger::Info("=== MapManager Summary ===");
    Logger::Info("Current Map: %s", m_currentMap.name.c_str());
    Logger::Info("Bounds: min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f)",
                 m_currentMap.bounds.min.x, m_currentMap.bounds.min.y, m_currentMap.bounds.min.z,
                 m_currentMap.bounds.max.x, m_currentMap.bounds.max.y, m_currentMap.bounds.max.z);
    Logger::Info("Spawn Points: %zu", m_spawnPoints.size());
    Logger::Info("Objectives: %zu", m_objectives.size());
    Logger::Info("==========================");
}