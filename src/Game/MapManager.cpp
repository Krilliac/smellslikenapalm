// src/Game/MapManager.cpp – Complete implementation for RS2V Server MapManager

#include "Game/MapManager.h"
#include "Utils/Logger.h"
#include "Config/MapConfig.h"
#include "Network/NetworkManager.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
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
    if (!m_mapConfig) {
        Logger::Error("MapManager: No map config available — cannot load map: %s", mapName.c_str());
        return false;
    }
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
    Logger::Trace("[MapManager::LoadGeometry] Entry: path='%s'", path.c_str());

    if (!std::filesystem::exists(path)) {
        Logger::Error("Map geometry file not found: %s", path.c_str());
        Logger::Trace("[MapManager::LoadGeometry] Exit: return false (file not found)");
        return false;
    }

    auto fileSize = std::filesystem::file_size(path);
    if (fileSize == 0) {
        Logger::Error("Map geometry file is empty: %s", path.c_str());
        return false;
    }

    // Open geometry file and try to parse bounds information
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        Logger::Error("Failed to open geometry file: %s", path.c_str());
        return false;
    }

    // Read the first 4 bytes as a magic number to identify file format
    char magic[4] = {};
    file.read(magic, sizeof(magic));
    if (!file.good()) {
        // File too small for header - use definition bounds
        file.close();
        m_bounds.min = m_currentMap.bounds.min;
        m_bounds.max = m_currentMap.bounds.max;
        Logger::Info("Loaded geometry (definition bounds) from %s (%.0f KB)",
                     path.c_str(), static_cast<float>(fileSize) / 1024.0f);
        return true;
    }

    // Check for UE3 package magic (0xC1832A9E) or use definition bounds
    uint32_t magicVal = 0;
    std::memcpy(&magicVal, magic, sizeof(uint32_t));

    if (magicVal == 0xC1832A9E) {
        Logger::Debug("[MapManager::LoadGeometry] Detected UE3 package format");

        // Skip package header fields: version(4) + licensee(4) + headerSize(4)
        file.seekg(12, std::ios::cur);

        // Attempt to read bounds from the expected offset
        float minX, minY, minZ, maxX, maxY, maxZ;
        file.read(reinterpret_cast<char*>(&minX), sizeof(float));
        file.read(reinterpret_cast<char*>(&minY), sizeof(float));
        file.read(reinterpret_cast<char*>(&minZ), sizeof(float));
        file.read(reinterpret_cast<char*>(&maxX), sizeof(float));
        file.read(reinterpret_cast<char*>(&maxY), sizeof(float));
        file.read(reinterpret_cast<char*>(&maxZ), sizeof(float));

        if (file.good()) {
            m_bounds.min = Vector3(minX, minY, minZ);
            m_bounds.max = Vector3(maxX, maxY, maxZ);
            Logger::Debug("[MapManager::LoadGeometry] UE3 bounds: min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f)",
                          minX, minY, minZ, maxX, maxY, maxZ);
        } else {
            m_bounds.min = m_currentMap.bounds.min;
            m_bounds.max = m_currentMap.bounds.max;
        }
    } else {
        // Unknown format - use definition bounds as fallback
        Logger::Debug("[MapManager::LoadGeometry] Unknown format (magic=0x%08X), using definition bounds", magicVal);
        m_bounds.min = m_currentMap.bounds.min;
        m_bounds.max = m_currentMap.bounds.max;
    }

    file.close();

    Logger::Info("Loaded geometry from %s (%.0f KB, bounds: min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f))",
                 path.c_str(), static_cast<float>(fileSize) / 1024.0f,
                 m_bounds.min.x, m_bounds.min.y, m_bounds.min.z,
                 m_bounds.max.x, m_bounds.max.y, m_bounds.max.z);
    Logger::Trace("[MapManager::LoadGeometry] Exit: return true");
    return true;
}

void MapManager::GenerateFallbackSpawns()
{
    Logger::Warn("Generating fallback spawn points");
    Bounds b = GetMapBounds();
    // Guard against degenerate/inverted bounds: std::uniform_real_distribution
    // has undefined behavior unless min <= max. Normalize before use.
    float minX = std::min(b.min.x, b.max.x), maxX = std::max(b.min.x, b.max.x);
    float minY = std::min(b.min.y, b.max.y), maxY = std::max(b.min.y, b.max.y);
    float minZ = std::min(b.min.z, b.max.z), maxZ = std::max(b.min.z, b.max.z);
    if (minX == maxX && minY == maxY && minZ == maxZ) {
        Logger::Warn("MapManager: Map bounds are degenerate/empty — fallback spawns will all be at the same point");
    }
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dx(minX, maxX);
    std::uniform_real_distribution<float> dy(minY, maxY);
    std::uniform_real_distribution<float> dz(minZ, maxZ);

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
    if (!m_mapConfig) {
        Logger::Warn("MapManager: No map config available — GetNextMap returning current map");
        return m_currentMap.name;
    }
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