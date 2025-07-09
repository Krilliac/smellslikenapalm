// src/Game/GameWorld.h – Header for GameWorld

#pragma once

#include <vector>
#include <map>
#include <memory>
#include "Math/Vector3.h"
#include "Game/DynamicObject.h"
#include "Game/SpawnPoint.h"
#include "Game/Bounds.h"

class GameServer;
class MapManager;
class PlayerManager;
class TeamManager;

class GameWorld {
public:
    GameWorld(GameServer* server,
              std::shared_ptr<MapManager> mapMgr,
              std::shared_ptr<PlayerManager> playerMgr,
              std::shared_ptr<TeamManager> teamMgr);
    ~GameWorld();

    bool Initialize();
    void Update(double deltaTime);

    void RespawnPlayer(uint32_t playerId);

    void AddDynamicObject(const DynamicObject& obj);
    void RemoveDynamicObject(uint32_t objectId);

    Bounds GetMapBounds() const;
    std::vector<Vector3> GetTeamSpawnPoints(uint32_t teamId) const;

    std::vector<uint8_t> SerializeSnapshot() const;

private:
    void UpdateEnvironmentalEffects(double deltaTime);
    void UpdateDynamicObjects(double deltaTime);

    GameServer* m_server;
    std::shared_ptr<MapManager>   m_mapManager;
    std::shared_ptr<PlayerManager> m_playerManager;
    std::shared_ptr<TeamManager>   m_teamManager;

    std::vector<SpawnPoint>       m_spawnPoints;
    std::map<uint32_t, size_t>    m_nextSpawnIndex;
    double                         m_elapsedTime{0.0};

    std::vector<DynamicObject>    m_dynamicObjects;
};