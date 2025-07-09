// src/Game/MapManager.h – Header for MapManager

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "Game/SpawnPoint.h"
#include "Game/Bounds.h"
#include "Config/MapConfig.h"

class GameServer;

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

private:
    bool LoadGeometry(const std::string& path);
    void GenerateFallbackSpawns();

    GameServer*                     m_server;
    std::shared_ptr<MapConfig>      m_mapConfig;
    MapDefinition                   m_currentMap;
    std::vector<SpawnPoint>         m_spawnPoints;
    std::vector<uint32_t>           m_objectives;
    Bounds                          m_bounds;
};