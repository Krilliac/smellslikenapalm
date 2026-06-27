// src/Game/MapManager.cpp – Complete implementation for RS2V Server MapManager

#include "Game/MapManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Config/MapConfig.h"
#include "Network/NetworkManager.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    // Spawn points: prefer an on-disk spawns.txt (per-map or global), then the
    // map definition's embedded list, then a random fallback within bounds.
    m_spawnPoints.clear();
    if (!LoadSpawnPointsFromDisk(mapName)) {
        for (const auto& pos : m_currentMap.spawnPoints) {
            SpawnPoint sp;
            sp.position = pos;
            sp.teamId = 0;
            m_spawnPoints.push_back(sp);
        }
    }
    if (m_spawnPoints.empty()) {
        // Fallback: random points in bounds
        GenerateFallbackSpawns();
    }
    Logger::Info("MapManager: %zu spawn points loaded", m_spawnPoints.size());

    // Per-map lighting overrides (lighting.json), if present.
    LoadLighting(mapName);

    // Objectives: prefer an on-disk objectives.txt with full positional capture
    // zones; otherwise fall back to the definition's id list / objectiveCount and
    // synthesize evenly-spread zones so the ObjectiveSystem still has real markers.
    m_objectives.clear();
    m_objectiveZones.clear();
    if (!LoadObjectivesFromDisk(mapName)) {
        if (!m_currentMap.objectiveIds.empty()) {
            for (int objId : m_currentMap.objectiveIds) {
                m_objectives.push_back(static_cast<uint32_t>(objId));
            }
        } else if (m_currentMap.objectiveCount > 0) {
            for (int i = 0; i < m_currentMap.objectiveCount; ++i) {
                m_objectives.push_back(static_cast<uint32_t>(i + 1));
            }
            Logger::Debug("MapManager: synthesized %d objective ids from objectiveCount",
                          m_currentMap.objectiveCount);
        }
        BuildObjectiveZones();
    }
    Logger::Info("MapManager: %zu objectives loaded (%zu positional zones)",
                 m_objectives.size(), m_objectiveZones.size());

    return true;
}

std::string MapManager::MapAssetDir(const std::string& mapName) const
{
    // The map asset's directory (where the .umap lives). Per-map auxiliary files
    // may live either directly beside the .umap or in a <mapName>/ subdirectory.
    std::filesystem::path p(m_currentMap.filePath);
    std::string base = p.has_parent_path() ? p.parent_path().string() : std::string(".");
    (void)mapName;
    return base;
}

bool MapManager::LoadSpawnPointsFromDisk(const std::string& mapName)
{
    namespace fs = std::filesystem;
    std::string baseDir = MapAssetDir(mapName);

    // Candidate locations, in priority order, matching data/maps/README.md.
    std::vector<std::string> candidates = {
        baseDir + "/" + mapName + "/spawns.txt",   // maps/<id>/spawns.txt
        baseDir + "/spawns.txt",                    // maps/spawns.txt (flat layout)
        baseDir + "/global_spawns.txt"              // shared fallback templates
    };

    for (const auto& path : candidates) {
        if (!fs::exists(path)) continue;

        std::ifstream f(path);
        if (!f.is_open()) {
            Logger::Warn("MapManager: spawns file exists but could not be opened: %s", path.c_str());
            continue;
        }

        size_t before = m_spawnPoints.size();
        std::string line;
        uint32_t autoId = 1;
        while (std::getline(f, line)) {
            // Format: "x y z [teamId]" (whitespace separated), '#' starts a comment.
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::istringstream iss(line);
            float x, y, z;
            if (!(iss >> x >> y >> z)) continue;  // skip blank/garbage lines
            int teamId = 0;
            iss >> teamId;                         // optional, defaults to 0/neutral

            SpawnPoint sp;
            sp.id = autoId++;
            sp.position = {x, y, z};
            sp.teamId = static_cast<uint32_t>(teamId);
            sp.enabled = true;
            m_spawnPoints.push_back(sp);
        }

        size_t added = m_spawnPoints.size() - before;
        if (added > 0) {
            Logger::Info("MapManager: loaded %zu spawn points from %s", added, path.c_str());
            return true;
        }
        Logger::Warn("MapManager: spawns file '%s' contained no valid points", path.c_str());
    }
    return false;
}

bool MapManager::LoadObjectivesFromDisk(const std::string& mapName)
{
    namespace fs = std::filesystem;
    std::string baseDir = MapAssetDir(mapName);

    // Candidate locations, in priority order, mirroring the spawns.txt scheme.
    std::vector<std::string> candidates = {
        baseDir + "/" + mapName + "/objectives.txt",   // maps/<id>/objectives.txt
        baseDir + "/objectives.txt",                    // maps/objectives.txt (flat)
        baseDir + "/global_objectives.txt"              // shared fallback templates
    };

    for (const auto& path : candidates) {
        if (!fs::exists(path)) continue;

        std::ifstream f(path);
        if (!f.is_open()) {
            Logger::Warn("MapManager: objectives file exists but could not be opened: %s", path.c_str());
            continue;
        }

        std::vector<CaptureZone> zones;
        std::string line;
        uint32_t autoId = 1;
        int order = 0;
        while (std::getline(f, line)) {
            // Format (whitespace separated, '#' starts a comment):
            //   name x y z [radius] [order] [tunnel 0/1] [tunnelX tunnelY tunnelZ]
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::istringstream iss(line);
            std::string name;
            float x, y, z;
            if (!(iss >> name >> x >> y >> z)) continue;  // skip blank/garbage lines

            CaptureZone zone;
            zone.id = autoId++;
            zone.name = name;
            zone.type = ObjectiveType::Territory;
            zone.position = {x, y, z};
            zone.isActive = true;

            float radius = 0.0f;
            if (iss >> radius && radius > 0.0f) zone.captureRadius = radius;

            int ord = order;
            if (iss >> ord) zone.territoryOrder = ord; else zone.territoryOrder = order;

            int tunnel = 0;
            if (iss >> tunnel && tunnel != 0) {
                zone.hasTunnel = true;
                float tx, ty, tz;
                if (iss >> tx >> ty >> tz) zone.tunnelPosition = {tx, ty, tz};
                else zone.tunnelPosition = zone.position;
            }

            zones.push_back(zone);
            ++order;
        }

        if (!zones.empty()) {
            m_objectiveZones = std::move(zones);
            m_objectives.clear();
            for (const auto& z : m_objectiveZones) m_objectives.push_back(z.id);
            Logger::Info("MapManager: loaded %zu objective zones from %s",
                         m_objectiveZones.size(), path.c_str());
            return true;
        }
        Logger::Warn("MapManager: objectives file '%s' contained no valid entries", path.c_str());
    }
    return false;
}

void MapManager::BuildObjectiveZones()
{
    // Synthesize positional capture zones from the bare objective id list so the
    // ObjectiveSystem has real in-world markers even when a map ships no
    // objectives.txt. Zones are spread evenly along the X axis within bounds.
    m_objectiveZones.clear();
    if (m_objectives.empty()) return;

    Bounds b = GetMapBounds();
    float minX = std::min(b.min.x, b.max.x), maxX = std::max(b.min.x, b.max.x);
    float midY = (b.min.y + b.max.y) * 0.5f;
    float midZ = (b.min.z + b.max.z) * 0.5f;
    size_t n = m_objectives.size();

    for (size_t i = 0; i < n; ++i) {
        CaptureZone zone;
        zone.id = m_objectives[i];
        zone.name = "Objective " + std::to_string(zone.id);
        zone.type = ObjectiveType::Territory;
        float t = (n == 1) ? 0.5f : static_cast<float>(i) / static_cast<float>(n - 1);
        zone.position = { minX + t * (maxX - minX), midY, midZ };
        zone.territoryOrder = static_cast<int>(i);
        zone.isActive = true;
        m_objectiveZones.push_back(zone);
    }
    Logger::Debug("MapManager: synthesized %zu positional objective zones from id list",
                  m_objectiveZones.size());
}

const std::vector<CaptureZone>& MapManager::GetObjectiveZones() const
{
    return m_objectiveZones;
}

void MapManager::LoadLighting(const std::string& mapName)
{
    namespace fs = std::filesystem;
    m_lighting = MapLighting{};
    // Seed defaults from the map definition's time_of_day.
    m_lighting.timeOfDay = m_currentMap.timeOfDay;

    std::string baseDir = MapAssetDir(mapName);
    std::vector<std::string> candidates = {
        baseDir + "/" + mapName + "/lighting.json",
        baseDir + "/lighting.json"
    };

    std::string path;
    for (const auto& c : candidates) {
        if (fs::exists(c)) { path = c; break; }
    }
    if (path.empty()) {
        Logger::Debug("MapManager: no lighting.json for '%s', using definition defaults", mapName.c_str());
        return;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        Logger::Warn("MapManager: lighting file exists but could not be opened: %s", path.c_str());
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string json = buf.str();

    // Tolerant, dependency-free extraction of the small documented schema:
    //   { "time_of_day": "dusk", "sun_intensity": 0.8, "ambient_color": [r,g,b] }
    auto findString = [&](const std::string& key) -> std::string {
        auto k = json.find("\"" + key + "\"");
        if (k == std::string::npos) return "";
        auto colon = json.find(':', k);
        if (colon == std::string::npos) return "";
        auto q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return "";
        auto q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return json.substr(q1 + 1, q2 - q1 - 1);
    };
    auto findNumber = [&](const std::string& key, float fallback) -> float {
        auto k = json.find("\"" + key + "\"");
        if (k == std::string::npos) return fallback;
        auto colon = json.find(':', k);
        if (colon == std::string::npos) return fallback;
        try {
            size_t idx = colon + 1;
            return std::stof(json.substr(idx), nullptr);
        } catch (...) { return fallback; }
    };

    std::string tod = findString("time_of_day");
    if (!tod.empty()) m_lighting.timeOfDay = tod;
    m_lighting.sunIntensity = findNumber("sun_intensity", m_lighting.sunIntensity);

    // ambient_color: parse the first three integers inside the array.
    auto ac = json.find("\"ambient_color\"");
    if (ac != std::string::npos) {
        auto lb = json.find('[', ac);
        auto rb = json.find(']', lb == std::string::npos ? ac : lb);
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            std::string inner = json.substr(lb + 1, rb - lb - 1);
            std::istringstream iss(inner);
            std::string tok;
            int comp = 0;
            while (std::getline(iss, tok, ',') && comp < 3) {
                try { m_lighting.ambientColor[comp++] = std::stoi(StringUtils::Trim(tok)); }
                catch (...) { /* leave default */ }
            }
        }
    }

    m_lighting.loaded = true;
    Logger::Info("MapManager: lighting loaded from %s (timeOfDay='%s', sun=%.2f, ambient=[%d,%d,%d])",
                 path.c_str(), m_lighting.timeOfDay.c_str(), m_lighting.sunIntensity,
                 m_lighting.ambientColor[0], m_lighting.ambientColor[1], m_lighting.ambientColor[2]);
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

        // UE3 package header (FPackageFileSummary): immediately after the tag
        // comes a packed version int (low 16 = file version, high 16 = licensee
        // version) followed by the total header size. We read them for logging /
        // sanity-checking; full export-table parsing is out of scope here.
        int32_t versionPacked = 0;
        int32_t totalHeaderSize = 0;
        file.read(reinterpret_cast<char*>(&versionPacked), sizeof(int32_t));
        file.read(reinterpret_cast<char*>(&totalHeaderSize), sizeof(int32_t));
        if (file.good()) {
            uint16_t fileVersion = static_cast<uint16_t>(versionPacked & 0xFFFF);
            uint16_t licenseeVersion = static_cast<uint16_t>((versionPacked >> 16) & 0xFFFF);
            Logger::Debug("[MapManager::LoadGeometry] UE3 header: fileVersion=%u licenseeVersion=%u headerSize=%d",
                          fileVersion, licenseeVersion, totalHeaderSize);
            if (fileVersion != 0 && fileVersion != 7258) {
                Logger::Warn("[MapManager::LoadGeometry] UE3 fileVersion %u != expected 7258 (RS2:V); map may be incompatible",
                             fileVersion);
            }
        }

        // Attempt to read a bounding box from the following offset. This is a
        // heuristic (real geometry lives in the export tables) but lets maps ship
        // an authored AABB right after the summary for server-side bounds checks.
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