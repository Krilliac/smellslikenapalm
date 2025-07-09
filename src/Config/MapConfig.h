// src/Config/MapConfig.h

#pragma once

#include <string>
#include <map>
#include <vector>

struct MapDefinition {
    std::string name;
    std::string displayName;
    std::string description;
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
};

class MapConfig {
public:
    MapConfig();
    ~MapConfig();

    // Load or create the map config file
    // Returns true on success
    bool Initialize(const std::string& configPath);

    // Reload definitions from disk
    bool Load();

    // Save current definitions to disk
    bool Save() const;

    // Create a default map definition and add it
    void CreateDefaultConfig();

    // Get a single map definition by name, or nullptr if not found
    const MapDefinition* GetDefinition(const std::string& name) const;

    // Get list of all available map names
    std::vector<std::string> GetAvailableMaps() const;

private:
    // Helper to apply a single key/value to a MapDefinition
    void ApplyProperty(MapDefinition& def, const std::string& key, const std::string& val);

    std::string                                    m_configPath;
    std::map<std::string, MapDefinition>           m_mapDefinitions;
};