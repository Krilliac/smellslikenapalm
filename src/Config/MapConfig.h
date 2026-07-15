// src/Config/MapConfig.h

#pragma once

#include <string>
#include <map>
#include <filesystem>
#include <set>
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
    std::string defaultMode;             // fallback ModeID when rotation omits one
    int         voteWeight        = 50;  // 1-100 likelihood weight for map voting

    // Spawn-point and configured bounds data. Cooked UE3 package bounds require
    // export-table parsing; MapManager does not infer them from summary bytes.
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
    // Production path: probe Steam's registered libraries automatically.
    explicit MapConfig(const ServerConfig& cfg);

    // Injected-only path: inspect exactly these roots. Passing an empty vector
    // disables discovery, keeping isolated tests independent of the host.
    MapConfig(const ServerConfig& cfg,
              std::vector<std::filesystem::path> retailDiscoveryRoots);
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

    // Resolve a maps.ini 'file' value to a usable path (relative to the maps
    // assets directory unless it is absolute or already has a directory).
    std::string ResolveMapFilePath(const std::string& file) const;

    // Directory that owns repo-local per-map/global auxiliary data such as
    // spawns.txt and objectives.txt. This can differ from an absolute retail
    // .roe asset's parent directory.
    const std::string& GetMapsDirectory() const { return m_mapsDir; }

private:
    void ApplyProperty(MapDefinition& def, const std::string& key, const std::string& val);

    std::string                                    m_mapsDir;
    std::string                                    m_rotationFile;
    std::string                                    m_configPath;
    bool                                           m_autoDiscoverRetailMaps = false;
    std::vector<std::filesystem::path>             m_retailDiscoveryRoots;
    std::set<std::string>                          m_discoveredDefinitionNames;
    std::map<std::string, MapDefinition>           m_mapDefinitions;
};
