// src/Config/MapConfig.h

#pragma once

#include <string>
#include <map>
#include <vector>
#include <optional>
#include "Math/Vector3.h"

struct MapDefinition {
    std::string name;
    std::string displayName;
    std::string description;
    std::string filePath;
    int         maxPlayers        = 0;
    int         minPlayers        = 0;
    std::string environment;
    std::string timeOfDay;
    std::string weather;
    std::string sizeCategory;
    bool        enableTunnels     = false;
    bool        enableVehicles    = false;
    int         objectiveCount    = 0;
    int         usTeamSpawns      = 0;
    int         nvaTeamSpawns     = 0;

    // Spawn point and bounds data (loaded from map file)
    std::vector<Vector3> spawnPoints;
    std::vector<int>     objectiveIds;
    struct Bounds { Vector3 min, max; } bounds = {};
    std::vector<std::string> supportedModes;
};

struct GameModeDefinition {
    std::string name;
    std::string displayName;
    int         roundTimeLimit = 900;
    int         scoreLimit     = 1000;
    int         respawnDelay   = 5;
    bool        friendlyFire   = false;
    bool        vehiclesEnabled = true;
};

struct GameSettings {
    std::string mapName;
    std::string gameMode = "Conquest";
    bool        friendlyFire = false;
    int         respawnDelay = 5;
    int         roundTimeLimit = 900;
    int         scoreLimit = 1000;
};

class ServerConfig;

class MapConfig {
public:
    explicit MapConfig(const ServerConfig& cfg);
    ~MapConfig();

    // Initialize (load from disk)
    bool Initialize();

    // Load/Save definitions
    bool Load();
    bool Save() const;

    // Create a default map definition
    void CreateDefaultConfig();

    // Get a single map definition by name, or nullptr if not found
    const MapDefinition* GetDefinition(const std::string& name) const;

    // Get list of all available map names
    std::vector<std::string> GetAvailableMaps() const;

private:
    void ApplyProperty(MapDefinition& def, const std::string& key, const std::string& val);

    std::string                                    m_mapsDir;
    std::string                                    m_configPath;
    std::map<std::string, MapDefinition>           m_mapDefinitions;
};
