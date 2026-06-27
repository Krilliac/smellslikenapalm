// src/Config/MapConfig.cpp

#include "Config/MapConfig.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <filesystem>
#include <sstream>

namespace {
// Normalize a directory path so it always ends in exactly one separator. The
// config values (data_directory, maps_path) are inconsistent about trailing
// slashes, so callers must not assume either way.
std::string EnsureTrailingSep(std::string s)
{
    if (!s.empty() && s.back() != '/' && s.back() != '\\') s += '/';
    return s;
}

// Resolve the maps assets directory from the (data_directory, maps_path) pair.
// maps_path is documented as "relative to server root"; some configs already
// embed the data directory in it (e.g. data_directory=data/, maps_path=data/maps/).
// Avoid doubling ("data/data/maps") by using maps_path as-is when it is absolute
// or already begins with the data directory; otherwise join it under data_directory.
std::string ResolveMapsDir(const std::string& dataDir, const std::string& mapsPath)
{
    if (mapsPath.empty()) return EnsureTrailingSep(dataDir);
    std::filesystem::path mp(mapsPath);
    std::string normDataDir = EnsureTrailingSep(dataDir);
    if (mp.is_absolute() || mapsPath.rfind(normDataDir, 0) == 0 ||
        mapsPath.rfind(dataDir, 0) == 0) {
        return EnsureTrailingSep(mapsPath);
    }
    return EnsureTrailingSep(normDataDir + mapsPath);
}
} // namespace

MapConfig::MapConfig(const ServerConfig& cfg)
  : m_mapsDir(ResolveMapsDir(cfg.GetDataDirectory(), cfg.GetMapsDataPath())),
    m_rotationFile(cfg.GetMapRotationFile())
{
    Logger::Info("MapConfig initialized with assets directory: %s", m_mapsDir.c_str());
}

MapConfig::~MapConfig() = default;

bool MapConfig::Initialize()
{
    // Prefer the explicitly configured rotation file (General.map_rotation_file,
    // default config/maps.ini — the populated, documented metadata file). Fall
    // back to maps.ini inside the assets directory for older layouts.
    if (!m_rotationFile.empty() && std::filesystem::exists(m_rotationFile)) {
        m_configPath = m_rotationFile;
    } else if (std::filesystem::exists(m_mapsDir + "maps.ini")) {
        m_configPath = m_mapsDir + "maps.ini";
    } else {
        // Nothing on disk yet — create a default next to the configured rotation
        // file if we have one, otherwise in the assets dir.
        m_configPath = !m_rotationFile.empty() ? m_rotationFile : (m_mapsDir + "maps.ini");
        Logger::Warn("MapConfig file not found, creating default: %s", m_configPath.c_str());
        CreateDefaultConfig();
        return Save();
    }

    Logger::Info("Initializing MapConfig from %s", m_configPath.c_str());
    return Load();
}

std::string MapConfig::ResolveMapFilePath(const std::string& file) const
{
    if (file.empty()) return file;
    std::filesystem::path p(file);
    // Absolute paths and paths already containing a directory component are used
    // as-is; bare filenames are resolved relative to the maps assets directory.
    if (p.is_absolute() || p.has_parent_path()) return file;
    return m_mapsDir + file;
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

    // Post-process: fill in derived defaults that the file may have omitted.
    for (auto& [name, d] : m_mapDefinitions) {
        if (d.filePath.empty()) {
            // No explicit 'file' key — assume <mapsDir>/<name>.umap by convention.
            d.filePath = m_mapsDir + name + ".umap";
        }
        if (d.defaultMode.empty() && !d.supportedModes.empty()) {
            d.defaultMode = d.supportedModes.front();
        }
        if (d.voteWeight < 1)   d.voteWeight = 1;
        if (d.voteWeight > 100) d.voteWeight = 100;
    }

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
        file << "display_name="    << def.displayName << "\n";
        file << "description="     << def.description << "\n";
        if (!def.filePath.empty())
            file << "file="            << def.filePath << "\n";
        if (!def.supportedModes.empty())
            file << "supported_modes=" << StringUtils::Join(def.supportedModes, ",") << "\n";
        file << "min_players="     << def.minPlayers << "\n";
        file << "max_players="     << def.maxPlayers << "\n";
        if (!def.defaultMode.empty())
            file << "default_mode="    << def.defaultMode << "\n";
        file << "environment="     << def.environment << "\n";
        file << "time_of_day="     << def.timeOfDay << "\n";
        file << "weather="         << def.weather << "\n";
        file << "size_category="   << def.sizeCategory << "\n";
        file << "enable_tunnels="  << (def.enableTunnels  ? "true" : "false") << "\n";
        file << "enable_vehicles=" << (def.enableVehicles ? "true" : "false") << "\n";
        file << "objective_count=" << def.objectiveCount << "\n";
        file << "us_team_spawns="  << def.usTeamSpawns << "\n";
        file << "nva_team_spawns=" << def.nvaTeamSpawns << "\n";
        file << "vote_weight="     << def.voteWeight << "\n";
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
    d.filePath       = ResolveMapFilePath("VNTE-CuChi.umap");
    d.supportedModes = {"Conquest", "Domination"};
    d.defaultMode    = "Conquest";
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
    d.voteWeight     = 50;

    m_mapDefinitions[d.name] = d;
}

void MapConfig::ApplyProperty(MapDefinition& def, const std::string& key, const std::string& val)
{
    // Accept both the documented snake_case keys (config/maps.ini) and the
    // legacy camelCase keys this class used to write, so old and new files load.
    auto safeInt = [&](const std::string& s, int fallback) -> int {
        auto v = StringUtils::ToInt(s);
        if (!v) {
            Logger::Warn("Map '%s': non-integer value '%s' for key '%s', using %d",
                         def.name.c_str(), s.c_str(), key.c_str(), fallback);
            return fallback;
        }
        return *v;
    };

    if      (key == "displayName"    || key == "display_name")    def.displayName    = val;
    else if (key == "description")                                def.description    = val;
    else if (key == "file")                                       def.filePath       = ResolveMapFilePath(val);
    else if (key == "maxPlayers"     || key == "max_players")     def.maxPlayers     = safeInt(val, def.maxPlayers);
    else if (key == "minPlayers"     || key == "min_players")     def.minPlayers     = safeInt(val, def.minPlayers);
    else if (key == "environment")                               def.environment    = val;
    else if (key == "timeOfDay"      || key == "time_of_day")     def.timeOfDay      = val;
    else if (key == "weather")                                   def.weather        = val;
    else if (key == "sizeCategory"   || key == "size_category")   def.sizeCategory   = val;
    else if (key == "enableTunnels"  || key == "enable_tunnels")  def.enableTunnels  = StringUtils::ToBool(val);
    else if (key == "enableVehicles" || key == "enable_vehicles") def.enableVehicles = StringUtils::ToBool(val);
    else if (key == "objectiveCount" || key == "objective_count") def.objectiveCount = safeInt(val, def.objectiveCount);
    else if (key == "usTeamSpawns"   || key == "us_team_spawns")  def.usTeamSpawns   = safeInt(val, def.usTeamSpawns);
    else if (key == "nvaTeamSpawns"  || key == "nva_team_spawns") def.nvaTeamSpawns  = safeInt(val, def.nvaTeamSpawns);
    else if (key == "defaultMode"    || key == "default_mode")    def.defaultMode    = val;
    else if (key == "voteWeight"     || key == "vote_weight")     def.voteWeight     = safeInt(val, def.voteWeight);
    else if (key == "supportedModes" || key == "supported_modes") {
        def.supportedModes.clear();
        for (auto& m : StringUtils::Split(val, ',')) {
            std::string t = StringUtils::Trim(m);
            if (!t.empty()) def.supportedModes.push_back(t);
        }
    }
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