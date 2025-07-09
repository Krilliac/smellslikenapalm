// src/Config/MapConfig.cpp

#include "Config/MapConfig.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <filesystem>
#include <sstream>

MapConfig::MapConfig(const ServerConfig& cfg)
  : m_mapsDir(cfg.GetDataDirectory() + "/" + cfg.GetMapsDataPath())
{
    Logger::Info("MapConfig initialized with assets directory: %s", m_mapsDir.c_str());
}

MapConfig::~MapConfig() = default;

bool MapConfig::Initialize()
{
    // Build full path to maps.ini in data/maps/
    m_configPath = m_mapsDir + "maps.ini";
    Logger::Info("Initializing MapConfig from %s", m_configPath.c_str());

    if (!std::filesystem::exists(m_configPath)) {
        Logger::Warn("MapConfig file not found, creating default: %s", m_configPath.c_str());
        CreateDefaultConfig();
        return Save();
    }
    return Load();
}

bool MapConfig::Load()
{
    std::ifstream file(m_configPath);
    if (!file.is_open()) {
        Logger::Error("Failed to open map config: %s", m_configPath.c_str());
        return false;
    }

    m_mapDefinitions.clear();
    std::string line, section;
    MapDefinition def;
    size_t lineNo = 0;

    while (std::getline(file, line)) {
        ++lineNo;
        line = StringUtils::Trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            if (!section.empty()) {
                m_mapDefinitions[section] = def;
                def = MapDefinition();
            }
            section = line.substr(1, line.size() - 2);
            def.name = section;
        } else {
            auto pos = line.find('=');
            if (pos == std::string::npos) {
                Logger::Warn("Invalid line %zu in %s: %s", lineNo, m_configPath.c_str(), line.c_str());
                continue;
            }
            std::string key = StringUtils::Trim(line.substr(0, pos));
            std::string val = StringUtils::Trim(line.substr(pos + 1));
            ApplyProperty(def, key, val);
        }
    }

    if (!section.empty()) {
        m_mapDefinitions[section] = def;
    }
    file.close();

    Logger::Info("Loaded %zu map definitions", m_mapDefinitions.size());
    return true;
}

bool MapConfig::Save() const
{
    std::ofstream file(m_configPath, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("Failed to write map config: %s", m_configPath.c_str());
        return false;
    }

    file << "# RS2V Map Configuration\n";
    for (const auto& [name, def] : m_mapDefinitions) {
        file << "\n[" << name << "]\n";
        file << "displayName="  << def.displayName << "\n";
        file << "description="  << def.description << "\n";
        file << "maxPlayers="   << def.maxPlayers << "\n";
        file << "minPlayers="   << def.minPlayers << "\n";
        file << "environment="  << def.environment << "\n";
        file << "timeOfDay="    << def.timeOfDay << "\n";
        file << "weather="      << def.weather << "\n";
        file << "sizeCategory=" << def.sizeCategory << "\n";
        file << "enableTunnels="  << (def.enableTunnels  ? "true" : "false") << "\n";
        file << "enableVehicles=" << (def.enableVehicles ? "true" : "false") << "\n";
        file << "objectiveCount=" << def.objectiveCount << "\n";
        file << "usTeamSpawns="   << def.usTeamSpawns << "\n";
        file << "nvaTeamSpawns="  << def.nvaTeamSpawns << "\n";
    }
    file.close();

    Logger::Info("Map configuration saved to %s", m_configPath.c_str());
    return true;
}

void MapConfig::CreateDefaultConfig()
{
    m_mapDefinitions.clear();

    MapDefinition d;
    d.name           = "VNTE-CuChi";
    d.displayName    = "Cu Chi";
    d.description    = "Underground tunnel complex";
    d.maxPlayers     = 64;
    d.minPlayers     = 16;
    d.environment    = "Jungle";
    d.timeOfDay      = "Day";
    d.weather        = "Clear";
    d.sizeCategory   = "Medium";
    d.enableTunnels  = true;
    d.enableVehicles = false;
    d.objectiveCount = 5;
    d.usTeamSpawns   = 3;
    d.nvaTeamSpawns  = 4;

    m_mapDefinitions[d.name] = d;
}

void MapConfig::ApplyProperty(MapDefinition& def, const std::string& key, const std::string& val)
{
    if      (key == "displayName")    def.displayName    = val;
    else if (key == "description")    def.description    = val;
    else if (key == "maxPlayers")     def.maxPlayers     = std::stoi(val);
    else if (key == "minPlayers")     def.minPlayers     = std::stoi(val);
    else if (key == "environment")    def.environment    = val;
    else if (key == "timeOfDay")      def.timeOfDay      = val;
    else if (key == "weather")        def.weather        = val;
    else if (key == "sizeCategory")   def.sizeCategory   = val;
    else if (key == "enableTunnels")  def.enableTunnels  = StringUtils::ToBool(val);
    else if (key == "enableVehicles") def.enableVehicles = StringUtils::ToBool(val);
    else if (key == "objectiveCount") def.objectiveCount = std::stoi(val);
    else if (key == "usTeamSpawns")   def.usTeamSpawns   = std::stoi(val);
    else if (key == "nvaTeamSpawns")  def.nvaTeamSpawns  = std::stoi(val);
    else {
        Logger::Debug("Unknown map property '%s' for map '%s'", key.c_str(), def.name.c_str());
    }
}

const MapDefinition* MapConfig::GetDefinition(const std::string& name) const
{
    auto it = m_mapDefinitions.find(name);
    return (it != m_mapDefinitions.end()) ? &it->second : nullptr;
}

std::vector<std::string> MapConfig::GetAvailableMaps() const
{
    std::vector<std::string> keys;
    keys.reserve(m_mapDefinitions.size());
    for (const auto& kv : m_mapDefinitions) {
        keys.push_back(kv.first);
    }
    return keys;
}