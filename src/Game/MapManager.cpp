// src/Game/MapManager.cpp – Complete implementation for RS2V Server MapManager

#include "Game/MapManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Config/MapConfig.h"
#include "Network/NetworkManager.h"
#include "Network/RetailBootstrap.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <random>

namespace {

// PACKAGE_FILE_TAG as it appears after decoding the on-disk little-endian
// bytes C1 83 2A 9E. The reversed-looking 0xC1832A9E value is the byte sequence,
// not the host-order integer stored by a Windows RS2 package.
constexpr uint32_t kUe3PackageTag = 0x9E2A83C1u;
constexpr uint16_t kRs2PackageFileVersion = 765u;
constexpr uint16_t kRs2PackageLicenseeVersion = 771u;
constexpr size_t kPackageSummaryPrefixSize = 12u;
// The fixed/required RS2 FPackageFileSummary fields extend well beyond the
// 12-byte tag/version/size prefix. This lower bound rejects a prefix-only file
// without pretending to parse the variable-length summary or export tables.
constexpr uint32_t kMinimumRs2PackageSummarySize = 64u;
constexpr size_t kMaximumBotNavigationNodes = 4096u;
constexpr size_t kMaximumBotNavigationArcs = 16384u;
constexpr size_t kMaximumBotNavigationLineLength = 4096u;

uint16_t ReadLittleEndian16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

uint32_t ReadLittleEndian32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool HasRoeExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".roe";
}

bool MapSupportsMode(const MapDefinition& map, const std::string& mode)
{
    if (StringUtils::EqualsIgnoreCase(map.defaultMode, mode)) return true;
    return std::any_of(map.supportedModes.begin(), map.supportedModes.end(),
                       [&](const std::string& supported) {
                           return StringUtils::EqualsIgnoreCase(supported, mode);
                       });
}

bool HasTrailingToken(std::istringstream& stream)
{
    std::string trailing;
    return static_cast<bool>(stream >> trailing);
}

} // namespace

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

    // Package validation can fail. Preserve the currently running map's
    // identity/effective bounds until the replacement geometry is known-good.
    const MapDefinition previousMap = m_currentMap;
    const Bounds previousBounds = m_bounds;
    m_currentMap = *def;

    // Load map geometry, collision, navmesh, etc.
    if (!LoadGeometry(m_currentMap.filePath)) {
        Logger::Error("MapManager: Failed to load geometry for %s", mapName.c_str());
        m_currentMap = previousMap;
        m_bounds = previousBounds;
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
    LoadObjectiveLockdownMetadata(mapName);
    Logger::Info("MapManager: %zu objectives loaded (%zu positional zones)",
                 m_objectives.size(), m_objectiveZones.size());

    // Navigation metadata is optional and map-local. Invalid or missing data
    // clears the previous map's graph and leaves GameServer's legacy bounded
    // direct-routing behavior intact.
    m_botNavigation = MapBotNavigationMetadata{};
    LoadBotNavigationMetadata(mapName);

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
    const fs::path assetDir(MapAssetDir(mapName));
    const fs::path configuredDir(m_mapConfig ? m_mapConfig->GetMapsDirectory() : std::string());

    // Candidate locations, in priority order, matching data/maps/README.md.
    // An absolute retail .roe path points outside this repo, so also search the
    // configured maps-data directory for the auxiliary files we own locally.
    std::vector<std::string> candidates = {
        (assetDir / mapName / "spawns.txt").string(),
        (assetDir / "spawns.txt").string(),
        (configuredDir / mapName / "spawns.txt").string(),
        (configuredDir / "spawns.txt").string(),
        (assetDir / "global_spawns.txt").string(),
        (configuredDir / "global_spawns.txt").string()
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
        size_t lineNo = 0;
        while (std::getline(f, line)) {
            ++lineNo;
            // Format: "x y z [teamId [minTerritoryPhase [maxTerritoryPhase
            //          [retailSpawnVolumeRef]]]]"
            // (whitespace separated), '#' starts a comment. Phase bounds are
            // inclusive and -1 means unbounded. The final optional value is the
            // static PackageMap object ref for the cooked spawn-group volume.
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::istringstream iss(line);
            float x, y, z;
            if (!(iss >> x >> y >> z)) continue;  // skip blank/garbage lines
            int teamId = 0;
            iss >> teamId;                         // optional, defaults to 0/neutral

            int minTerritoryPhase = -1;
            int maxTerritoryPhase = -1;
            uint32_t retailSpawnVolumeRef = 0;
            if (iss >> minTerritoryPhase) {
                iss >> maxTerritoryPhase;
                const bool invalidMin = minTerritoryPhase < -1;
                const bool invalidMax = maxTerritoryPhase < -1;
                const bool reversed = minTerritoryPhase >= 0 &&
                                      maxTerritoryPhase >= 0 &&
                                      maxTerritoryPhase < minTerritoryPhase;
                if (invalidMin || invalidMax || reversed) {
                    Logger::Warn("MapManager: %s:%zu has invalid Territory phase bounds "
                                 "min=%d max=%d; treating spawn as unbounded",
                                 path.c_str(), lineNo,
                                 minTerritoryPhase, maxTerritoryPhase);
                    minTerritoryPhase = -1;
                    maxTerritoryPhase = -1;
                }
                uint64_t parsedRef = 0;
                if (iss >> parsedRef) {
                    if (parsedRef >= 0x80000000ull) {
                        Logger::Warn("MapManager: %s:%zu has invalid static spawn-volume "
                                     "reference %llu; retail spawn-map entry disabled",
                                     path.c_str(), lineNo,
                                     static_cast<unsigned long long>(parsedRef));
                    } else {
                        retailSpawnVolumeRef = static_cast<uint32_t>(parsedRef);
                    }
                }
            }

            SpawnPoint sp;
            sp.id = autoId++;
            sp.position = {x, y, z};
            sp.teamId = static_cast<uint32_t>(teamId);
            sp.enabled = true;
            sp.minTerritoryPhase = minTerritoryPhase;
            sp.maxTerritoryPhase = maxTerritoryPhase;
            sp.retailSpawnVolumeRef = retailSpawnVolumeRef;
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
    const fs::path assetDir(MapAssetDir(mapName));
    const fs::path configuredDir(m_mapConfig ? m_mapConfig->GetMapsDirectory() : std::string());

    // Candidate locations, in priority order, mirroring the spawns.txt scheme.
    // Search repo-local configured data as well as beside an absolute retail asset.
    std::vector<std::string> candidates = {
        (assetDir / mapName / "objectives.txt").string(),
        (assetDir / "objectives.txt").string(),
        (configuredDir / mapName / "objectives.txt").string(),
        (configuredDir / "objectives.txt").string(),
        (assetDir / "global_objectives.txt").string(),
        (configuredDir / "global_objectives.txt").string()
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
        size_t lineNo = 0;
        std::array<bool, 16> usedClientSlots{};
        std::set<uint8_t> usedCookedRepIndices;
        while (std::getline(f, line)) {
            ++lineNo;
            // Format (whitespace separated, '#' starts a comment):
            //   name x y z [radius] [phase] [tunnel 0/1]
            //        [tunnelX tunnelY tunnelZ]
            //        [clientSlot cookedRepIndex] [enabled 0/1]
            //        [connectedToBase 0/1] [initialOwner 0..2] [captureSeconds]
            //        [pointValue] [homeTeam 0..2] [adjacentClientSlotsCsv]
            //
            // Names may be quoted when they contain whitespace. std::quoted also
            // accepts the legacy single-token spelling without quotes.
            // The retail metadata tail is optional so legacy/global objective
            // files remain valid. A metadata-bearing non-tunnel row must include
            // the explicit tunnel value 0 before clientSlot/cookedRepIndex.
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::istringstream iss(line);
            std::string name;
            float x, y, z;
            if (!(iss >> std::quoted(name) >> x >> y >> z)) continue;  // skip blank/garbage lines

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
            if (iss >> tunnel) {
                if (tunnel != 0) {
                    zone.hasTunnel = true;
                    float tx, ty, tz;
                    if (iss >> tx >> ty >> tz) zone.tunnelPosition = {tx, ty, tz};
                    else zone.tunnelPosition = zone.position;
                }

                int clientSlot = -1;
                if (iss >> clientSlot) {
                    int cookedRepIndex = -1;
                    if (!(iss >> cookedRepIndex)) {
                        Logger::Warn("MapManager: %s:%zu objective '%s' specifies clientSlot "
                                     "without cookedRepIndex; leaving it unmapped",
                                     path.c_str(), lineNo, zone.name.c_str());
                    } else if (clientSlot < 0 || clientSlot >= 16 ||
                               cookedRepIndex < 0 || cookedRepIndex >= 255) {
                        Logger::Warn("MapManager: %s:%zu objective '%s' has invalid retail "
                                     "mapping slot=%d repIndex=%d (slot 0..15, repIndex 0..254); "
                                     "leaving it unmapped",
                                     path.c_str(), lineNo, zone.name.c_str(),
                                     clientSlot, cookedRepIndex);
                    } else if (usedClientSlots[static_cast<size_t>(clientSlot)] ||
                               usedCookedRepIndices.count(static_cast<uint8_t>(cookedRepIndex)) != 0) {
                        Logger::Warn("MapManager: %s:%zu objective '%s' duplicates retail "
                                     "slot=%d or repIndex=%d; first mapping wins and this zone "
                                     "remains unmapped",
                                     path.c_str(), lineNo, zone.name.c_str(),
                                     clientSlot, cookedRepIndex);
                    } else {
                        zone.clientSlot = static_cast<uint8_t>(clientSlot);
                        zone.cookedRepIndex = static_cast<uint8_t>(cookedRepIndex);
                        usedClientSlots[static_cast<size_t>(clientSlot)] = true;
                        usedCookedRepIndices.insert(zone.cookedRepIndex);

                        int enabled = 1;
                        if (iss >> enabled) {
                            if (enabled == 0 || enabled == 1) {
                                zone.enabled = enabled != 0;
                            } else {
                                Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                             "enabled=%d; using true",
                                             path.c_str(), lineNo, zone.name.c_str(), enabled);
                            }

                            int connectedToBase = 0;
                            if (iss >> connectedToBase) {
                                if (connectedToBase == 0 || connectedToBase == 1) {
                                    zone.connectedToBase = connectedToBase != 0;
                                } else {
                                    Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                                 "connectedToBase=%d; using false",
                                                 path.c_str(), lineNo, zone.name.c_str(),
                                                 connectedToBase);
                                }

                                int initialOwner = 0;
                                if (iss >> initialOwner) {
                                    if (initialOwner >= 0 && initialOwner <= 2) {
                                        zone.controllingTeam = static_cast<uint32_t>(initialOwner);
                                        zone.state = initialOwner == 0
                                            ? CaptureState::Neutral
                                            : CaptureState::Controlled;
                                    } else {
                                        Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                                     "initialOwner=%d; using neutral",
                                                     path.c_str(), lineNo, zone.name.c_str(),
                                                     initialOwner);
                                    }

                                    float captureSeconds = 0.0f;
                                    if (iss >> captureSeconds) {
                                        if (std::isfinite(captureSeconds) && captureSeconds > 0.0f) {
                                            zone.captureSpeed = 1.0f / captureSeconds;
                                        } else {
                                            Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                                         "captureSeconds=%.3f; using %.3f seconds",
                                                         path.c_str(), lineNo, zone.name.c_str(),
                                                         captureSeconds, 1.0f / zone.captureSpeed);
                                        }

                                        int pointValue = 1;
                                        if (iss >> pointValue) {
                                            if (pointValue >= 1 && pointValue <= 255) {
                                                zone.supremacyPointValue =
                                                    static_cast<uint8_t>(pointValue);
                                            } else {
                                                Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                                             "pointValue=%d; using 1",
                                                             path.c_str(), lineNo, zone.name.c_str(),
                                                             pointValue);
                                            }

                                            int homeTeam = 0;
                                            if (iss >> homeTeam) {
                                                if (homeTeam >= 0 && homeTeam <= 2) {
                                                    zone.supremacyHomeTeam =
                                                        static_cast<uint8_t>(homeTeam);
                                                } else {
                                                    Logger::Warn("MapManager: %s:%zu objective '%s' has invalid "
                                                                 "homeTeam=%d; using 0",
                                                                 path.c_str(), lineNo, zone.name.c_str(),
                                                                 homeTeam);
                                                }

                                                std::string adjacentSlots;
                                                if (iss >> adjacentSlots && adjacentSlots != "-") {
                                                    std::istringstream adjacentStream(adjacentSlots);
                                                    std::string slotToken;
                                                    while (std::getline(adjacentStream, slotToken, ',')) {
                                                        int adjacentSlot = -1;
                                                        std::istringstream slotStream(slotToken);
                                                        char trailing = '\0';
                                                        if (!(slotStream >> adjacentSlot) ||
                                                            (slotStream >> trailing) ||
                                                            adjacentSlot < 0 || adjacentSlot >= 16) {
                                                            Logger::Warn("MapManager: %s:%zu objective '%s' has "
                                                                         "invalid adjacent client slot '%s'; ignoring it",
                                                                         path.c_str(), lineNo, zone.name.c_str(),
                                                                         slotToken.c_str());
                                                            continue;
                                                        }
                                                        zone.supremacyAdjacentSlots |=
                                                            static_cast<uint16_t>(1u << adjacentSlot);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
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

bool MapManager::LoadObjectiveLockdownMetadata(const std::string& mapName)
{
    namespace fs = std::filesystem;
    const fs::path assetDir(MapAssetDir(mapName));
    const fs::path configuredDir(m_mapConfig ? m_mapConfig->GetMapsDirectory()
                                             : std::string());

    // Lockdown values are cooked ROObjective properties and are package-build
    // specific. Deliberately do not search a global fallback: applying another
    // map's class/default metadata would fabricate an early-win rule.
    const std::array<fs::path, 3> candidates = {{
        assetDir / mapName / "objective_lockdown.txt",
        assetDir / "objective_lockdown.txt",
        configuredDir / mapName / "objective_lockdown.txt"
    }};

    struct LockdownRow {
        bool enabled = false;
        std::array<int32_t, 3> times{{0, 0, 0}};
    };

    for (const fs::path& path : candidates) {
        std::error_code existsError;
        const bool exists = fs::exists(path, existsError);
        if (existsError || !exists) continue;

        std::ifstream file(path);
        if (!file.is_open()) {
            Logger::Warn("MapManager: objective lockdown file exists but could not be opened: %s",
                         path.string().c_str());
            return false;
        }

        std::array<bool, 16> seen{};
        std::array<LockdownRow, 16> rows{};
        size_t rowCount = 0;
        size_t lineNo = 0;
        bool invalid = false;
        std::string line;
        while (std::getline(file, line)) {
            ++lineNo;
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);

            std::istringstream input(line);
            input >> std::ws;
            if (input.eof()) continue;

            int64_t slot = -1;
            int64_t enabled = -1;
            int64_t time16 = -1;
            int64_t time32 = -1;
            int64_t time64 = -1;
            std::string trailing;
            if (!(input >> slot >> enabled >> time16 >> time32 >> time64) ||
                (input >> trailing) ||
                slot < 0 || slot >= static_cast<int64_t>(seen.size()) ||
                (enabled != 0 && enabled != 1) ||
                time16 < 0 || time16 > 86400 ||
                time32 < 0 || time32 > 86400 ||
                time64 < 0 || time64 > 86400) {
                Logger::Warn("MapManager: %s:%zu has invalid objective lockdown metadata; "
                             "rejecting the file",
                             path.string().c_str(), lineNo);
                invalid = true;
                break;
            }

            const size_t slotIndex = static_cast<size_t>(slot);
            if (seen[slotIndex]) {
                Logger::Warn("MapManager: %s:%zu duplicates objective client slot %lld; "
                             "rejecting the file",
                             path.string().c_str(), lineNo,
                             static_cast<long long>(slot));
                invalid = true;
                break;
            }

            seen[slotIndex] = true;
            rows[slotIndex].enabled = enabled != 0;
            rows[slotIndex].times = {{
                static_cast<int32_t>(time16),
                static_cast<int32_t>(time32),
                static_cast<int32_t>(time64)
            }};
            ++rowCount;
        }

        if (!invalid && rowCount == 0) {
            Logger::Warn("MapManager: objective lockdown file '%s' contained no metadata",
                         path.string().c_str());
            invalid = true;
        }

        std::array<CaptureZone*, 16> zonesBySlot{};
        if (!invalid) {
            for (CaptureZone& zone : m_objectiveZones) {
                if (zone.clientSlot < zonesBySlot.size()) {
                    zonesBySlot[zone.clientSlot] = &zone;
                }
            }
            for (size_t slot = 0; slot < seen.size(); ++slot) {
                if (seen[slot] && zonesBySlot[slot] == nullptr) {
                    Logger::Warn("MapManager: objective lockdown file '%s' references unmapped "
                                 "client slot %zu; rejecting the file",
                                 path.string().c_str(), slot);
                    invalid = true;
                    break;
                }
            }
        }

        if (invalid) {
            // Parsing is all-or-nothing so a partially corrupted package-data
            // sidecar cannot arm only part of a grouped Territory phase.
            return false;
        }

        for (size_t slot = 0; slot < seen.size(); ++slot) {
            if (!seen[slot]) continue;
            CaptureZone& zone = *zonesBySlot[slot];
            zone.lockdownMetadataKnown = true;
            zone.lockdownEnabled = rows[slot].enabled;
            zone.lockdownTimeSecondsByPlayerBand = rows[slot].times;
        }

        Logger::Info("MapManager: loaded %zu objective lockdown rows from %s",
                     rowCount, path.string().c_str());
        return true;
    }

    return false;
}

bool MapManager::LoadBotNavigationMetadata(const std::string& mapName)
{
    namespace fs = std::filesystem;
    const fs::path assetDir(MapAssetDir(mapName));
    const fs::path configuredDir(m_mapConfig ? m_mapConfig->GetMapsDirectory()
                                             : std::string());
    const std::array<fs::path, 3> candidates = {{
        assetDir / mapName / "bot_navigation.txt",
        assetDir / "bot_navigation.txt",
        configuredDir / mapName / "bot_navigation.txt"
    }};

    for (const fs::path& path : candidates) {
        std::error_code existsError;
        if (!fs::exists(path, existsError)) {
            if (existsError) {
                Logger::Warn("MapManager: could not inspect bot navigation sidecar '%s': %s",
                             path.string().c_str(), existsError.message().c_str());
                return false;
            }
            continue;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            Logger::Warn("MapManager: bot navigation sidecar exists but could not be opened: %s",
                         path.string().c_str());
            return false;
        }

        MapBotNavigationMetadata parsed;
        bool sawVersion = false;
        bool sawMode = false;
        bool sawConfig = false;
        bool invalid = false;
        size_t lineNo = 0;
        size_t directedArcCount = 0;
        std::set<std::uint32_t> nodeIds;
        std::set<std::pair<std::uint32_t, std::uint32_t>> directedArcs;
        std::string line;

        const auto reject = [&](const char* reason) {
            Logger::Warn("MapManager: invalid bot navigation sidecar %s:%zu (%s); "
                         "rejecting the whole graph",
                         path.string().c_str(), lineNo, reason);
            invalid = true;
        };

        while (!invalid && std::getline(file, line)) {
            ++lineNo;
            if (line.size() > kMaximumBotNavigationLineLength) {
                reject("line length limit exceeded");
                continue;
            }
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            line = StringUtils::Trim(line);
            if (line.empty()) continue;

            std::istringstream stream(line);
            std::string directive;
            if (!(stream >> directive)) continue;

            if (directive == "version") {
                int version = 0;
                if (sawVersion || !(stream >> version) || version != 1 ||
                    HasTrailingToken(stream)) {
                    reject("expected exactly one 'version 1' row");
                    continue;
                }
                sawVersion = true;
                continue;
            }

            if (directive == "mode") {
                std::string mode;
                if (sawMode || !(stream >> mode) || mode.empty() ||
                    HasTrailingToken(stream)) {
                    reject("expected exactly one single-token mode row");
                    continue;
                }
                parsed.mode = std::move(mode);
                sawMode = true;
                continue;
            }

            if (directive == "config") {
                int allowDirectFallback = -1;
                BotNavigationConfig config;
                if (sawConfig ||
                    !(stream >> config.maxEdgeLength >>
                      config.maxEndpointSnapDistance >>
                      config.maxDirectRouteLength >> allowDirectFallback) ||
                    HasTrailingToken(stream) ||
                    (allowDirectFallback != 0 && allowDirectFallback != 1)) {
                    reject("malformed config row");
                    continue;
                }
                config.enforceBounds = false;
                parsed.config = config;
                parsed.allowDirectFallback = allowDirectFallback != 0;
                sawConfig = true;
                continue;
            }

            if (directive == "node") {
                std::uint64_t id = 0;
                BotWaypointNode node;
                if (!(stream >> id >> node.position.x >> node.position.y >>
                      node.position.z) || HasTrailingToken(stream) || id == 0 ||
                    id > std::numeric_limits<std::uint32_t>::max()) {
                    reject("malformed node row");
                    continue;
                }
                node.id = static_cast<std::uint32_t>(id);
                if (!nodeIds.insert(node.id).second) {
                    reject("duplicate node id");
                    continue;
                }
                if (parsed.graph.nodes.size() >= kMaximumBotNavigationNodes) {
                    reject("node limit exceeded");
                    continue;
                }
                parsed.graph.nodes.push_back(node);
                continue;
            }

            if (directive == "edge") {
                std::uint64_t from = 0;
                std::uint64_t to = 0;
                int bidirectional = -1;
                if (!(stream >> from >> to >> bidirectional) ||
                    HasTrailingToken(stream) || from == 0 || to == 0 || from == to ||
                    from > std::numeric_limits<std::uint32_t>::max() ||
                    to > std::numeric_limits<std::uint32_t>::max() ||
                    (bidirectional != 0 && bidirectional != 1)) {
                    reject("malformed edge row");
                    continue;
                }

                BotWaypointEdge edge;
                edge.fromId = static_cast<std::uint32_t>(from);
                edge.toId = static_cast<std::uint32_t>(to);
                edge.bidirectional = bidirectional != 0;
                const auto forward = std::make_pair(edge.fromId, edge.toId);
                const auto reverse = std::make_pair(edge.toId, edge.fromId);
                if (!directedArcs.insert(forward).second ||
                    (edge.bidirectional && !directedArcs.insert(reverse).second)) {
                    reject("duplicate directed edge");
                    continue;
                }
                directedArcCount += edge.bidirectional ? 2u : 1u;
                if (directedArcCount > kMaximumBotNavigationArcs) {
                    reject("directed edge limit exceeded");
                    continue;
                }
                parsed.graph.edges.push_back(edge);
                continue;
            }

            reject("unknown directive");
        }

        if (!invalid && file.bad()) {
            ++lineNo;
            reject("I/O error while reading sidecar");
        }

        if (!invalid && (!sawVersion || !sawMode || !sawConfig)) {
            ++lineNo;
            reject("missing required version, mode, or config row");
        }
        if (!invalid && (parsed.graph.nodes.empty() || parsed.graph.edges.empty())) {
            ++lineNo;
            reject("graph must contain at least one node and edge");
        }
        if (!invalid && !MapSupportsMode(m_currentMap, parsed.mode)) {
            ++lineNo;
            reject("declared mode is not supported by this map");
        }
        if (!invalid) {
            for (const BotWaypointEdge& edge : parsed.graph.edges) {
                if (nodeIds.find(edge.fromId) == nodeIds.end() ||
                    nodeIds.find(edge.toId) == nodeIds.end()) {
                    ++lineNo;
                    reject("edge references a missing node");
                    break;
                }
            }
        }

        BotNavigation validator;
        if (!invalid &&
            (!validator.Configure(parsed.config) ||
             !validator.ReplaceGraph(parsed.graph))) {
            ++lineNo;
            reject("graph violates navigation configuration");
        }
        if (invalid) return false;

        parsed.loaded = true;
        parsed.sourcePath = path.string();
        m_botNavigation = std::move(parsed);
        Logger::Info("MapManager: loaded %zu bot navigation nodes / %zu directed arcs "
                     "for mode '%s' from %s",
                     m_botNavigation.graph.nodes.size(), directedArcCount,
                     m_botNavigation.mode.c_str(), path.string().c_str());
        return true;
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
        } catch (...) {
            // Malformed numeric value in the map lighting file. Surface it so a
            // mapper can fix the data instead of silently getting the default.
            Logger::Warn("[MapManager] lighting key '%s' has a non-numeric value; using default %.2f",
                         key.c_str(), fallback);
            return fallback;
        }
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
                catch (...) {
                    // Bad component in the ambient_color array; keep the default
                    // for this channel but tell the mapper which token was wrong.
                    Logger::Warn("[MapManager] ambient_color component %d ('%s') is not an integer; keeping default",
                                 comp, StringUtils::Trim(tok).c_str());
                }
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

    // A cooked UE3 package does not store an AABB immediately after its summary
    // prefix. Until the export tables are parsed properly, configured bounds are
    // the only trustworthy bounds source. Set the fallback before every I/O exit
    // so a failed load cannot leak the previous map's effective bounds.
    const Vector3& configuredMin = m_currentMap.bounds.min;
    const Vector3& configuredMax = m_currentMap.bounds.max;
    const bool finiteBounds = std::isfinite(configuredMin.x) &&
                              std::isfinite(configuredMin.y) &&
                              std::isfinite(configuredMin.z) &&
                              std::isfinite(configuredMax.x) &&
                              std::isfinite(configuredMax.y) &&
                              std::isfinite(configuredMax.z);
    if (finiteBounds) {
        // Normalize inverted authored axes once so every bounds consumer sees
        // the same effective AABB.
        m_bounds.min = {
            std::min(configuredMin.x, configuredMax.x),
            std::min(configuredMin.y, configuredMax.y),
            std::min(configuredMin.z, configuredMax.z)
        };
        m_bounds.max = {
            std::max(configuredMin.x, configuredMax.x),
            std::max(configuredMin.y, configuredMax.y),
            std::max(configuredMin.z, configuredMax.z)
        };
    } else {
        // Invalid configured floats must not reach uniform_real_distribution.
        m_bounds = {};
        Logger::Warn("MapManager: Non-finite configured bounds for %s; using the origin",
                     m_currentMap.name.c_str());
    }

    std::error_code fsError;
    if (!std::filesystem::exists(path, fsError) || fsError) {
        Logger::Error("Map geometry file not found: %s", path.c_str());
        Logger::Trace("[MapManager::LoadGeometry] Exit: return false (file not found)");
        return false;
    }

    const auto fileSize = std::filesystem::file_size(path, fsError);
    if (fsError) {
        Logger::Error("Failed to inspect map geometry file %s: %s",
                      path.c_str(), fsError.message().c_str());
        return false;
    }
    if (fileSize == 0) {
        Logger::Error("Map geometry file is empty: %s", path.c_str());
        return false;
    }

    // Open the geometry file only far enough to validate its package-summary
    // prefix. Bounds require real export-table parsing and stay configured.
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        Logger::Error("Failed to open geometry file: %s", path.c_str());
        return false;
    }

    std::array<uint8_t, kPackageSummaryPrefixSize> summaryPrefix{};
    file.read(reinterpret_cast<char*>(summaryPrefix.data()),
              static_cast<std::streamsize>(summaryPrefix.size()));
    if (file.gcount() != static_cast<std::streamsize>(summaryPrefix.size())) {
        Logger::Error("Map geometry header is truncated: %s (%llu bytes; need at least %zu)",
                      path.c_str(), static_cast<unsigned long long>(fileSize),
                      summaryPrefix.size());
        return false;
    }

    const uint32_t magic = ReadLittleEndian32(summaryPrefix.data());
    if (magic != kUe3PackageTag) {
        if (HasRoeExtension(path)) {
            Logger::Error("Invalid RS2 .roe package tag in %s: 0x%08X (expected 0x%08X)",
                          path.c_str(), magic, kUe3PackageTag);
            return false;
        }

        // Preserve support for explicitly configured non-.roe geometry formats.
        // No bytes from an unknown format are interpreted as authoritative bounds.
        Logger::Warn("Unknown map geometry format in %s (magic=0x%08X); using configured bounds",
                     path.c_str(), magic);
    } else {
        const uint16_t fileVersion = ReadLittleEndian16(summaryPrefix.data() + 4u);
        const uint16_t licenseeVersion = ReadLittleEndian16(summaryPrefix.data() + 6u);
        const uint32_t totalHeaderSize = ReadLittleEndian32(summaryPrefix.data() + 8u);

        Logger::Debug("[MapManager::LoadGeometry] UE3 header: fileVersion=%u licenseeVersion=%u headerSize=%u",
                      static_cast<unsigned>(fileVersion),
                      static_cast<unsigned>(licenseeVersion),
                      static_cast<unsigned>(totalHeaderSize));

        if (fileVersion != kRs2PackageFileVersion ||
            licenseeVersion != kRs2PackageLicenseeVersion) {
            Logger::Error("Unsupported RS2 package version in %s: file=%u licensee=%u "
                          "(expected file=%u licensee=%u)",
                          path.c_str(), static_cast<unsigned>(fileVersion),
                          static_cast<unsigned>(licenseeVersion),
                          static_cast<unsigned>(kRs2PackageFileVersion),
                          static_cast<unsigned>(kRs2PackageLicenseeVersion));
            return false;
        }

        if (totalHeaderSize < kMinimumRs2PackageSummarySize || totalHeaderSize > fileSize) {
            Logger::Error("Invalid UE3 header size in %s: %u (file size %llu)",
                          path.c_str(), static_cast<unsigned>(totalHeaderSize),
                          static_cast<unsigned long long>(fileSize));
            return false;
        }

        Logger::Debug("[MapManager::LoadGeometry] Validated RS2:V UE3 package prefix metadata; "
                      "using configured bounds (export-table bounds parsing not implemented)");
    }

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
    return m_bounds;
}

std::string MapManager::GetNextMap()
{
    // Cycle once through the configured order, but only return maps whose
    // PackageMap/Welcome profile is source-grounded. Runtime travel cannot use
    // ResolveProfile's legacy Resort fallback without desynchronizing clients.
    if (!m_mapConfig) {
        Logger::Warn("MapManager: No map config available - GetNextMap has no safe target");
        return {};
    }

    const std::vector<std::string> maps = m_mapConfig->GetAvailableMaps();
    if (maps.empty()) return {};

    const auto current = std::find(maps.begin(), maps.end(), m_currentMap.name);
    const size_t start = current == maps.end()
        ? 0u
        : (static_cast<size_t>(std::distance(maps.begin(), current)) + 1u) % maps.size();

    for (size_t offset = 0; offset < maps.size(); ++offset) {
        const std::string& candidate = maps[(start + offset) % maps.size()];
        if (candidate == m_currentMap.name) continue;
        if (RetailBootstrap::HasExactProfile(candidate)) return candidate;
        Logger::Debug(
            "MapManager: Skipping rotation map '%s' because no exact retail "
            "bootstrap profile is available",
            candidate.c_str());
    }

    Logger::Warn(
        "MapManager: No exact-profile successor is available after '%s'; "
        "rotation will stay on the current map",
        m_currentMap.name.c_str());
    return {};
}

void MapManager::LogSummary() const
{
    Logger::Info("=== MapManager Summary ===");
    Logger::Info("Current Map: %s", m_currentMap.name.c_str());
    Logger::Info("Bounds: min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f)",
                 m_bounds.min.x, m_bounds.min.y, m_bounds.min.z,
                 m_bounds.max.x, m_bounds.max.y, m_bounds.max.z);
    Logger::Info("Spawn Points: %zu", m_spawnPoints.size());
    Logger::Info("Objectives: %zu", m_objectives.size());
    Logger::Info("==========================");
}
