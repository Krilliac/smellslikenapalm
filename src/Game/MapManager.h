// src/Game/MapManager.h – Header for MapManager

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "Game/SpawnPoint.h"
#include "Game/Bounds.h"
#include "Config/MapConfig.h"

class GameServer;

// Per-map lighting / time-of-day overrides, optionally loaded from lighting.json
// alongside the map asset. All fields fall back to map-definition defaults when
// no lighting file is present.
struct MapLighting {
    bool        loaded        = false;
    std::string timeOfDay;                 // day | dusk | night | dawn
    float       sunIntensity  = 1.0f;
    int         ambientColor[3] = {255, 255, 255};
};

class MapManager {
public:
    MapManager(GameServer* server, std::shared_ptr<MapConfig> mapConfig);
    ~MapManager();

    bool LoadMap(const std::string& mapName);
    std::vector<SpawnPoint> GetSpawnPoints() const;
    std::vector<uint32_t>  GetMapObjectives() const;
    Bounds                 GetMapBounds() const;
    std::string            GetNextMap();
    void                   LogSummary() const;

    // Current map accessors (valid after a successful LoadMap)
    const MapDefinition&   GetCurrentMap() const { return m_currentMap; }
    const std::string&     GetCurrentMapName() const { return m_currentMap.name; }
    const MapLighting&     GetLighting() const { return m_lighting; }

private:
    bool LoadGeometry(const std::string& path);
    void GenerateFallbackSpawns();
    bool LoadSpawnPointsFromDisk(const std::string& mapName);
    void LoadLighting(const std::string& mapName);
    std::string MapAssetDir(const std::string& mapName) const;

    GameServer*                     m_server;
    std::shared_ptr<MapConfig>      m_mapConfig;
    MapDefinition                   m_currentMap;
    std::vector<SpawnPoint>         m_spawnPoints;
    std::vector<uint32_t>           m_objectives;
    Bounds                          m_bounds;
    MapLighting                     m_lighting;
};
