// tests/MapRuntimeTests.cpp
//
// Focused runtime coverage for the current MapConfig/MapManager APIs. In
// production, maps.ini may point at an absolute retail .roe package while
// server-authored spawn metadata remains under DataPaths.maps_path.

#include "TestFramework.h"

#include "Config/ConfigManager.h"
#include "Config/MapConfig.h"
#include "Config/RetailMapDiscovery.h"
#include "Config/ServerConfig.h"
#include "Game/MapManager.h"
#include "Game/MapVoteManager.h"
#include "Game/ObjectiveSystem.h"
#include "Game/SpawnSystem.h"
#include "Game/TeamMapping.h"
#include "Network/RetailBootstrap.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace {

class MapRuntimeTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = std::filesystem::temp_directory_path() /
                 ("rs2v_map_runtime_" + std::to_string(unique));
        m_mapsDir = m_root / "server_data" / "maps";
        m_externalDir = m_root / "retail_maps" / "Resort";
        std::filesystem::create_directories(m_mapsDir);
        std::filesystem::create_directories(m_externalDir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    static void WriteText(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << text;
    }

    static std::string ReadText(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    static std::string EscapeKeyValuesPath(const std::filesystem::path& path) {
        std::string escaped;
        for (const char ch : path.string()) {
            if (ch == '\\' || ch == '"') escaped.push_back('\\');
            escaped.push_back(ch);
        }
        return escaped;
    }

    static void WriteSteamMetadata(const std::filesystem::path& steamRoot,
                                   const std::filesystem::path& libraryRoot,
                                   std::string_view installDirectory =
                                       "Rising Storm 2") {
        WriteText(
            steamRoot / "steamapps" / "libraryfolders.vdf",
            "\"libraryfolders\"\n"
            "{\n"
            "  \"0\"\n"
            "  {\n"
            "    \"path\" \"" + EscapeKeyValuesPath(steamRoot) + "\"\n"
            "  }\n"
            "  \"1\"\n"
            "  {\n"
            "    \"path\" \"" + EscapeKeyValuesPath(libraryRoot) + "\"\n"
            "    \"apps\" { \"418460\" \"1\" }\n"
            "  }\n"
            "}\n");
        WriteText(
            libraryRoot / "steamapps" / "appmanifest_418460.acf",
            "\"AppState\"\n"
            "{\n"
            "  \"appid\" \"418460\"\n"
            "  \"installdir\" \"" + std::string(installDirectory) + "\"\n"
            "}\n");
    }

    static void WriteRetailPackage(
        const std::filesystem::path& path,
        uint16_t fileVersion = 765u,
        uint16_t licenseeVersion = 771u,
        uint32_t declaredHeaderSize = 64u,
        const std::array<float, 6>* fakePostPrefixBounds = nullptr,
        uint32_t packageTag = 0x9E2A83C1u) {
        std::array<uint8_t, 64> bytes{};
        const auto put16 = [&](size_t offset, uint16_t value) {
            bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
            bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
        };
        const auto put32 = [&](size_t offset, uint32_t value) {
            bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
            bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
            bytes[offset + 2u] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
            bytes[offset + 3u] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
        };

        put32(0u, packageTag);
        put16(4u, fileVersion);
        put16(6u, licenseeVersion);
        put32(8u, declaredHeaderSize);
        if (fakePostPrefixBounds != nullptr) {
            std::memcpy(bytes.data() + 12u, fakePostPrefixBounds->data(),
                        sizeof(float) * fakePostPrefixBounds->size());
        }

        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    static void WriteTruncatedRetailPrefix(const std::filesystem::path& path) {
        const std::array<uint8_t, 8> bytes = {
            0xC1u, 0x83u, 0x2Au, 0x9Eu, // UE3 package tag on disk
            0xFDu, 0x02u, 0x03u, 0x03u  // file 765, licensee 771
        };
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    std::shared_ptr<MapConfig> MakeMapConfig(
        std::string_view mapId,
        const std::filesystem::path& mapAsset,
        std::string_view extraProperties = {}) {
        const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
        std::string rotation =
            "[" + std::string(mapId) + "]\n"
            "display_name=Package Test\n"
            "file=" + mapAsset.generic_string() + "\n"
            "default_mode=Territories\n"
            "supported_modes=Territories\n";
        rotation.append(extraProperties.data(), extraProperties.size());
        WriteText(rotationFile, rotation);

        auto configManager = std::make_shared<ConfigManager>();
        configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
        configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
        configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

        ServerConfig serverConfig(configManager);
        auto mapConfig = std::make_shared<MapConfig>(
            serverConfig, std::vector<std::filesystem::path>{});
        if (!mapConfig->Initialize()) return nullptr;
        return mapConfig;
    }

    std::filesystem::path m_root;
    std::filesystem::path m_mapsDir;
    std::filesystem::path m_externalDir;
};

TEST_F(MapRuntimeTest, AbsoluteAssetUsesConfiguredGlobalSpawnFallback) {
    constexpr std::string_view mapId = "VNTE-AbsoluteTest";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-AbsoluteTest.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    // Crucially, no spawns.txt is placed beside this external retail asset.
    WriteRetailPackage(mapAsset);

    WriteText(m_mapsDir / "global_spawns.txt",
              "125 250 375 1\n"
              "-625 -750 875 2\n");
    WriteText(rotationFile,
              "[VNTE-AbsoluteTest]\n"
              "display_name=Absolute Test\n"
              "file=" + mapAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());
    EXPECT_TRUE(std::filesystem::equivalent(
        std::filesystem::path(mapConfig->GetMapsDirectory()), m_mapsDir));

    const MapDefinition* definition = mapConfig->GetDefinition(std::string(mapId));
    ASSERT_TRUE(definition != nullptr);
    EXPECT_EQ(std::filesystem::path(definition->filePath).lexically_normal(),
              mapAsset.lexically_normal());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto spawns = manager.GetSpawnPoints();
    ASSERT_EQ(spawns.size(), 2u);

    EXPECT_EQ(spawns[0].teamId, 1u);
    EXPECT_EQ(spawns[0].minTerritoryPhase, -1);
    EXPECT_EQ(spawns[0].maxTerritoryPhase, -1);
    EXPECT_FLOAT_EQ(spawns[0].position.x, 125.0f);
    EXPECT_FLOAT_EQ(spawns[0].position.y, 250.0f);
    EXPECT_FLOAT_EQ(spawns[0].position.z, 375.0f);

    EXPECT_EQ(spawns[1].teamId, 2u);
    EXPECT_EQ(spawns[1].minTerritoryPhase, -1);
    EXPECT_EQ(spawns[1].maxTerritoryPhase, -1);
    EXPECT_FLOAT_EQ(spawns[1].position.x, -625.0f);
    EXPECT_FLOAT_EQ(spawns[1].position.y, -750.0f);
    EXPECT_FLOAT_EQ(spawns[1].position.z, 875.0f);
}

TEST_F(MapRuntimeTest,
       SteamDiscoveryAddsExactRetailFallbackWithoutWritingConfig) {
    const std::filesystem::path steamRoot = m_root / "steam";
    const std::filesystem::path libraryRoot = m_root / "steam-library";
    WriteSteamMetadata(steamRoot, libraryRoot);

    const std::filesystem::path retailMaps =
        libraryRoot / "steamapps" / "common" / "Rising Storm 2" /
        "ROGame" / "BrewedPC" / "Maps";
    const std::filesystem::path retailPackage =
        retailMaps / "CuChi" / "VNTE-CuChi.roe";
    WriteRetailPackage(retailPackage);
    WriteRetailPackage(retailMaps / "Resort" / "VNTE-Resort.roe");
    WriteRetailPackage(retailMaps / "HueCity" / "VNSU-HueCity.roe");
    WriteRetailPackage(retailMaps / "Compound" / "VNSK-Compound.roe");
    WriteText(m_mapsDir / "VNTE-CuChi" / "spawns.txt", "1 2 3 1\n");
    WriteText(m_mapsDir / "VNTE-CuChi" / "objectives.txt",
              "Alpha 10 20 30 35 0 0\n");
    std::filesystem::create_directories(m_mapsDir / "VNTE-Resort");
    std::filesystem::create_directories(m_mapsDir / "VNSU-HueCity");
    std::filesystem::create_directories(m_mapsDir / "VNSK-Compound");

    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    WriteText(rotationFile,
              "[LocalOnly]\n"
              "display_name=Explicit local entry\n"
              "file=missing.umap\n"
              "default_mode=Conquest\n"
              "supported_modes=Conquest\n");
    const std::string originalConfig = ReadText(rotationFile);

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file",
                             rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{steamRoot});

    ASSERT_TRUE(mapConfig->Initialize());
    EXPECT_EQ(ReadText(rotationFile), originalConfig);
    const MapDefinition* discovered = mapConfig->GetDefinition("VNTE-CuChi");
    ASSERT_TRUE(discovered != nullptr);
    EXPECT_EQ(discovered->defaultMode, std::string("Territories"));
    ASSERT_EQ(discovered->supportedModes.size(), 1u);
    EXPECT_EQ(discovered->supportedModes[0], std::string("Territories"));
    EXPECT_TRUE(std::filesystem::equivalent(discovered->filePath,
                                            retailPackage));
    ASSERT_TRUE(mapConfig->GetDefinition("VNTE-Resort") != nullptr);
    EXPECT_EQ(mapConfig->GetDefinition("VNTE-Resort")->defaultMode,
              std::string("Territories"));
    ASSERT_TRUE(mapConfig->GetDefinition("VNSU-HueCity") != nullptr);
    EXPECT_EQ(mapConfig->GetDefinition("VNSU-HueCity")->defaultMode,
              std::string("Supremacy"));
    ASSERT_TRUE(mapConfig->GetDefinition("VNSK-Compound") != nullptr);
    EXPECT_EQ(mapConfig->GetDefinition("VNSK-Compound")->defaultMode,
              std::string("Skirmish"));
    EXPECT_EQ(mapConfig->GetAvailableMaps().size(), 5u);

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap("VNTE-CuChi"));
    EXPECT_EQ(manager.GetSpawnPoints().size(), 1u);
    EXPECT_EQ(manager.GetMapObjectives().size(), 1u);

    // Even an explicit Save request persists only operator-authored sections.
    ASSERT_TRUE(mapConfig->Save());
    const std::string savedConfig = ReadText(rotationFile);
    EXPECT_TRUE(savedConfig.find("[LocalOnly]") != std::string::npos);
    EXPECT_TRUE(savedConfig.find("[VNTE-CuChi]") == std::string::npos);
    EXPECT_TRUE(savedConfig.find("[VNTE-Resort]") == std::string::npos);
    EXPECT_TRUE(savedConfig.find("[VNSU-HueCity]") == std::string::npos);
    EXPECT_TRUE(savedConfig.find("[VNSK-Compound]") == std::string::npos);
    EXPECT_TRUE(savedConfig.find(retailPackage.string()) == std::string::npos);
}

TEST_F(MapRuntimeTest, ExplicitRetailDefinitionWinsOverSteamDiscovery) {
    const std::filesystem::path steamRoot = m_root / "steam";
    const std::filesystem::path libraryRoot = m_root / "steam-library";
    WriteSteamMetadata(steamRoot, libraryRoot);
    const std::filesystem::path discoveredPackage =
        libraryRoot / "steamapps" / "common" / "Rising Storm 2" /
        "ROGame" / "BrewedPC" / "Maps" / "CuChi" / "VNTE-CuChi.roe";
    WriteRetailPackage(discoveredPackage);

    const std::filesystem::path explicitPackage =
        std::filesystem::absolute(m_externalDir / "VNTE-CuChi.roe");
    WriteRetailPackage(explicitPackage);
    WriteText(m_mapsDir / "VNTE-CuChi" / "spawns.txt", "1 2 3 1\n");

    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    WriteText(rotationFile,
              "[VNTE-CuChi]\n"
              "display_name=Operator Cu Chi\n"
              "file=" + explicitPackage.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");
    const std::string originalConfig = ReadText(rotationFile);

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file",
                             rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    MapConfig mapConfig(
        serverConfig, std::vector<std::filesystem::path>{steamRoot});

    ASSERT_TRUE(mapConfig.Initialize());
    const MapDefinition* definition = mapConfig.GetDefinition("VNTE-CuChi");
    ASSERT_TRUE(definition != nullptr);
    EXPECT_EQ(definition->displayName, std::string("Operator Cu Chi"));
    EXPECT_TRUE(std::filesystem::equivalent(definition->filePath,
                                            explicitPackage));
    EXPECT_EQ(ReadText(rotationFile), originalConfig);
}

TEST_F(MapRuntimeTest,
       DiscoveryExcludesUnsupportedMapsAndGeometryValidationStillFailsClosed) {
    const std::filesystem::path steamRoot = m_root / "steam";
    const std::filesystem::path libraryRoot = m_root / "steam-library";
    WriteSteamMetadata(steamRoot, libraryRoot);
    const std::filesystem::path retailMaps =
        libraryRoot / "steamapps" / "common" / "Rising Storm 2" /
        "ROGame" / "BrewedPC" / "Maps";
    WriteRetailPackage(retailMaps / "Hill937" / "VNTE-Hill937.roe");
    WriteRetailPackage(retailMaps / "RungSac" / "VNTE-RungSac.roe");
    WriteTruncatedRetailPrefix(retailMaps / "CuChi" / "VNTE-CuChi.roe");
    WriteText(m_mapsDir / "VNTE-CuChi" / "spawns.txt", "1 2 3 1\n");

    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    WriteText(rotationFile,
              "[LocalOnly]\n"
              "file=missing.umap\n"
              "default_mode=Conquest\n");
    const std::string originalConfig = ReadText(rotationFile);

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file",
                             rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{steamRoot});

    ASSERT_TRUE(mapConfig->Initialize());
    EXPECT_TRUE(mapConfig->GetDefinition("VNTE-CuChi") != nullptr);
    EXPECT_TRUE(mapConfig->GetDefinition("VNTE-Hill937") == nullptr);
    EXPECT_TRUE(mapConfig->GetDefinition("VNTE-RungSac") == nullptr);
    EXPECT_EQ(ReadText(rotationFile), originalConfig);

    MapManager manager(nullptr, mapConfig);
    EXPECT_FALSE(manager.LoadMap("VNTE-CuChi"));
    EXPECT_TRUE(manager.GetCurrentMapName().empty());
}

TEST_F(MapRuntimeTest, MalformedAndAmbiguousSteamManifestsFailClosed) {
    const auto writeInstalledCuChi = [&](const std::filesystem::path& root,
                                         std::string_view installDirectory) {
        const std::filesystem::path package =
            root / "steamapps" / "common" / installDirectory /
            "ROGame" / "BrewedPC" / "Maps" / "CuChi" /
            "VNTE-CuChi.roe";
        WriteRetailPackage(package);
    };

    const std::filesystem::path malformedRoot = m_root / "malformed-steam";
    writeInstalledCuChi(malformedRoot, "Rising Storm 2");
    WriteText(malformedRoot / "steamapps" / "appmanifest_418460.acf",
              "\"AppState\"\n"
              "{\n"
              "  \"appid\" \"418460\"\n"
              "  \"installdir\" \"Rising Storm 2\"\n");
    EXPECT_TRUE(RetailMapDiscovery::DiscoverFromSteamRoots({malformedRoot})
                    .empty());

    const std::filesystem::path ambiguousRoot = m_root / "ambiguous-steam";
    writeInstalledCuChi(ambiguousRoot, "Rising Storm 2");
    WriteText(ambiguousRoot / "steamapps" / "appmanifest_418460.acf",
              "\"AppState\"\n"
              "{\n"
              "  \"appid\" \"418460\"\n"
              "  \"installdir\" \"Rising Storm 2\"\n"
              "  \"installdir\" \"Conflicting Install\"\n"
              "}\n");
    EXPECT_TRUE(RetailMapDiscovery::DiscoverFromSteamRoots({ambiguousRoot})
                    .empty());
}

TEST_F(MapRuntimeTest, RotationSkipsMapsWithoutExactRetailProfiles) {
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    const std::filesystem::path resortAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-Resort.roe");
    const std::filesystem::path compoundAsset =
        std::filesystem::absolute(m_externalDir / "VNSK-Compound.roe");
    WriteRetailPackage(resortAsset);
    WriteRetailPackage(compoundAsset);

    const auto section = [](std::string_view map,
                            const std::filesystem::path& asset,
                            std::string_view mode) {
        std::string value = "[" + std::string(map) + "]\n";
        value += "display_name=" + std::string(map) + "\n";
        value += "file=" + asset.generic_string() + "\n";
        value += "default_mode=" + std::string(mode) + "\n";
        value += "supported_modes=" + std::string(mode) + "\n\n";
        return value;
    };
    const std::string unsupportedOnlyRotation =
        section("VNTE-Hill937", m_externalDir / "VNTE-Hill937.roe",
                "Territories") +
        section("VNTE-Resort", resortAsset, "Territories") +
        section("VNTE-RungSac", m_externalDir / "VNTE-RungSac.roe",
                "Territories");
    WriteText(rotationFile, unsupportedOnlyRotation);

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString(
        "General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString(
        "General.map_rotation_file", rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap("VNTE-Resort"));
    // Starting after Resort visits RungSac, wraps to Hill937, then reaches the
    // current map. Neither unsupported target may be selected.
    EXPECT_TRUE(manager.GetNextMap().empty());
    MapVoteManager unsupportedVote(mapConfig);
    unsupportedVote.SetOptionCount(8);
    EXPECT_TRUE(unsupportedVote.StartVote("VNTE-Resort").empty());
    EXPECT_FALSE(unsupportedVote.IsVoteActive());

    // Adding one exact-profile successor makes rotation deterministic while
    // leaving both unsupported maps in the configured list.
    WriteText(
        rotationFile,
        section("VNSK-Compound", compoundAsset, "Skirmish") +
            unsupportedOnlyRotation);
    ASSERT_TRUE(mapConfig->Initialize());
    EXPECT_EQ(manager.GetNextMap(), std::string("VNSK-Compound"));

    MapVoteManager exactVote(mapConfig);
    exactVote.SetOptionCount(8);
    const auto& candidates = exactVote.StartVote("VNTE-Resort");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().mapName, std::string("VNSK-Compound"));
    EXPECT_TRUE(
        RetailBootstrap::HasExactProfile(candidates.front().mapName));

    ASSERT_TRUE(manager.LoadMap("VNSK-Compound"));
    // Hill937 and RungSac have grounded cooked GUIDs but no exact bootstrap
    // profiles, so neither can enter bootstrap-driven rotation.
    EXPECT_EQ(manager.GetNextMap(), std::string("VNTE-Resort"));

    for (const char* current : {"VNTE-Resort", "VNSK-Compound"}) {
        ASSERT_TRUE(manager.LoadMap(current));
        const std::string target = manager.GetNextMap();
        ASSERT_FALSE(target.empty()) << current;
        const MapDefinition* definition = mapConfig->GetDefinition(target);
        ASSERT_TRUE(definition != nullptr) << target;

        const std::optional<RetailBootstrap::Profile> profile =
            RetailBootstrap::ResolveExactProfile(target, definition->defaultMode);
        const std::optional<RetailBootstrap::GuidBytes> guid =
            RetailBootstrap::ResolveMapPackageGuid(target);
        ASSERT_TRUE(profile.has_value()) << target;
        ASSERT_TRUE(guid.has_value()) << target;
        EXPECT_FALSE(profile->usedFallback) << target;
        EXPECT_EQ(profile->mapUrl, target);
        EXPECT_EQ(profile->mapPackageGuid, *guid) << target;
    }
}

TEST_F(MapRuntimeTest, RetailPackageHeaderDoesNotMasqueradeAsMapBounds) {
    constexpr std::string_view mapId = "VNTE-HeaderBounds";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-HeaderBounds.roe");

    // These six floats occupy the exact bytes the old heuristic treated as an
    // AABB. They are ordinary package-summary data from MapManager's point of
    // view and must never become authoritative bounds.
    const std::array<float, 6> fakeBounds = {
        -900.0f, -800.0f, -700.0f,
         900.0f,  800.0f,  700.0f
    };
    WriteRetailPackage(mapAsset, 765u, 771u, 64u, &fakeBounds);

    auto mapConfig = MakeMapConfig(
        mapId, mapAsset,
        "bounds_min=12345,23456,34567\n"
        "bounds_max=-12345,-23456,-34567\n");
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const Bounds bounds = manager.GetMapBounds();
    // Raw authored values remain available in the definition, while callers
    // receive MapManager's normalized effective AABB.
    EXPECT_FLOAT_EQ(manager.GetCurrentMap().bounds.min.x, 12345.0f);
    EXPECT_FLOAT_EQ(bounds.min.x, -12345.0f);
    EXPECT_FLOAT_EQ(bounds.min.y, -23456.0f);
    EXPECT_FLOAT_EQ(bounds.min.z, -34567.0f);
    EXPECT_FLOAT_EQ(bounds.max.x, 12345.0f);
    EXPECT_FLOAT_EQ(bounds.max.y, 23456.0f);
    EXPECT_FLOAT_EQ(bounds.max.z, 34567.0f);
}

TEST_F(MapRuntimeTest, RetailPackageVersionIsNotNetworkEngineVersion) {
    constexpr std::string_view mapId = "VNTE-WrongPackageVersion";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-WrongPackageVersion.roe");

    // 7258 is RS2's network engine version, not its UE3 package file version.
    WriteRetailPackage(mapAsset, 7258u, 771u);
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));
}

TEST_F(MapRuntimeTest, RetailPackageRejectsWrongLicenseeVersion) {
    constexpr std::string_view mapId = "VNTE-WrongLicenseeVersion";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-WrongLicenseeVersion.roe");

    WriteRetailPackage(mapAsset, 765u, 770u);
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));
}

TEST_F(MapRuntimeTest, RetailPackageRejectsByteReversedTagValue) {
    constexpr std::string_view mapId = "VNTE-WrongPackageTag";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-WrongPackageTag.roe");

    // 0xC1832A9E describes the four file bytes when read left-to-right, but is
    // not the little-endian integer value of UE3's PACKAGE_FILE_TAG.
    WriteRetailPackage(mapAsset, 765u, 771u, 64u, nullptr, 0xC1832A9Eu);
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));
}

TEST_F(MapRuntimeTest, RetailPackageRejectsImpossibleHeaderSizes) {
    constexpr std::string_view mapId = "VNTE-BadHeaderSize";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-BadHeaderSize.roe");

    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    WriteRetailPackage(mapAsset, 765u, 771u, 12u); // prefix only, not a summary
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));

    WriteRetailPackage(mapAsset, 765u, 771u, 65u); // fixture is exactly 64 bytes
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));
}

TEST_F(MapRuntimeTest, RetailPackageRejectsTruncatedSummaryPrefix) {
    constexpr std::string_view mapId = "VNTE-TruncatedHeader";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-TruncatedHeader.roe");

    WriteTruncatedRetailPrefix(mapAsset);
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    EXPECT_FALSE(manager.LoadMap(std::string(mapId)));
}

TEST_F(MapRuntimeTest, RetailPackageWithoutAuthoredBoundsUsesHonestZeroAabb) {
    constexpr std::string_view mapId = "VNTE-NoAuthoredBounds";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-NoAuthoredBounds.roe");

    WriteRetailPackage(mapAsset);
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));
    const Bounds bounds = manager.GetMapBounds();
    EXPECT_EQ(bounds.min, Vector3::Zero());
    EXPECT_EQ(bounds.max, Vector3::Zero());
}

TEST_F(MapRuntimeTest, FailedRetailPackageValidationKeepsCurrentMapState) {
    constexpr std::string_view validMapId = "VNTE-ValidCurrent";
    constexpr std::string_view invalidMapId = "VNTE-InvalidReplacement";
    const std::filesystem::path validAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-ValidCurrent.roe");
    const std::filesystem::path invalidAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-InvalidReplacement.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    WriteRetailPackage(validAsset);
    WriteRetailPackage(invalidAsset, 7258u, 771u);
    WriteText(rotationFile,
              "[VNTE-ValidCurrent]\n"
              "display_name=Valid Current\n"
              "file=" + validAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n"
              "bounds_min=-10,-20,-30\n"
              "bounds_max=10,20,30\n"
              "\n"
              "[VNTE-InvalidReplacement]\n"
              "display_name=Invalid Replacement\n"
              "file=" + invalidAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n"
              "bounds_min=-100,-200,-300\n"
              "bounds_max=100,200,300\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(validMapId)));
    const Bounds previousBounds = manager.GetMapBounds();
    const auto previousSpawns = manager.GetSpawnPoints();

    EXPECT_FALSE(manager.LoadMap(std::string(invalidMapId)));
    EXPECT_EQ(manager.GetCurrentMapName(), std::string(validMapId));
    EXPECT_EQ(manager.GetMapBounds().min, previousBounds.min);
    EXPECT_EQ(manager.GetMapBounds().max, previousBounds.max);
    EXPECT_EQ(manager.GetSpawnPoints().size(), previousSpawns.size());
}

TEST_F(MapRuntimeTest, SpawnTerritoryPhaseBoundsAreParsedCompatibly) {
    constexpr std::string_view mapId = "VNTE-PhaseSpawns";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-PhaseSpawns.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    WriteRetailPackage(mapAsset);

    WriteText(m_mapsDir / std::string(mapId) / "spawns.txt",
              "0 0 0 1\n"          // legacy, unbounded
              "10 0 0 1 0 0 301195\n" // phase zero + cooked spawn volume
              "20 0 0 1 1 3\n"     // inclusive phase range
              "30 0 0 2 2\n"       // phase two onward
              "40 0 0 2 4 3\n");   // invalid range becomes unbounded
    WriteText(rotationFile,
              "[VNTE-PhaseSpawns]\n"
              "display_name=Phase Spawns\n"
              "file=" + mapAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto spawns = manager.GetSpawnPoints();
    ASSERT_EQ(spawns.size(), 5u);
    EXPECT_EQ(spawns[0].minTerritoryPhase, -1);
    EXPECT_EQ(spawns[0].maxTerritoryPhase, -1);
    EXPECT_EQ(spawns[1].minTerritoryPhase, 0);
    EXPECT_EQ(spawns[1].maxTerritoryPhase, 0);
    EXPECT_EQ(spawns[1].retailSpawnVolumeRef, 301195u);
    EXPECT_EQ(spawns[2].minTerritoryPhase, 1);
    EXPECT_EQ(spawns[2].maxTerritoryPhase, 3);
    EXPECT_EQ(spawns[3].minTerritoryPhase, 2);
    EXPECT_EQ(spawns[3].maxTerritoryPhase, -1);
    EXPECT_EQ(spawns[4].minTerritoryPhase, -1);
    EXPECT_EQ(spawns[4].maxTerritoryPhase, -1);
}

TEST(SpawnRuntime, TerritoryPhaseBoundsAreInclusive) {
    SpawnLocation unbounded;
    EXPECT_FALSE(unbounded.HasTerritoryPhaseBounds());
    EXPECT_TRUE(unbounded.IsAvailableInTerritoryPhase(0));
    EXPECT_TRUE(unbounded.IsAvailableInTerritoryPhase(99));

    SpawnLocation bounded;
    bounded.minTerritoryPhase = 1;
    bounded.maxTerritoryPhase = 3;
    EXPECT_TRUE(bounded.HasTerritoryPhaseBounds());
    EXPECT_FALSE(bounded.IsAvailableInTerritoryPhase(-1));
    EXPECT_FALSE(bounded.IsAvailableInTerritoryPhase(0));
    EXPECT_TRUE(bounded.IsAvailableInTerritoryPhase(1));
    EXPECT_TRUE(bounded.IsAvailableInTerritoryPhase(2));
    EXPECT_TRUE(bounded.IsAvailableInTerritoryPhase(3));
    EXPECT_FALSE(bounded.IsAvailableInTerritoryPhase(4));
}

TEST(ObjectiveRuntime, TerritoryRoundResetRestoresDefenderOwnedFirstPhase) {
    ObjectiveSystem objectives(nullptr);
    objectives.Initialize();

    CaptureZone beach;
    beach.id = 1;
    beach.name = "Beach";
    beach.type = ObjectiveType::Territory;
    beach.territoryOrder = 0;
    CaptureZone villa;
    villa.id = 2;
    villa.name = "Villa";
    villa.type = ObjectiveType::Territory;
    villa.territoryOrder = 1;
    CaptureZone farm = villa;
    farm.id = 3;
    farm.name = "Farm";
    CaptureZone auxiliary = villa;
    auxiliary.id = 4;
    auxiliary.name = "Server Auxiliary";
    auxiliary.territoryOrder = 0;

    objectives.AddObjective(beach);
    objectives.AddObjective(villa);
    objectives.AddObjective(farm);
    objectives.AddObjective(auxiliary);
    objectives.SetTerritoryOrder({1, 2, 3});

    CaptureZone* capturedBeach = objectives.GetObjective(1);
    ASSERT_TRUE(capturedBeach != nullptr);
    capturedBeach->controllingTeam = 1;
    capturedBeach->captureProgress = 1.0f;
    objectives.ActivateNextTerritory(1);
    ASSERT_TRUE(objectives.GetCurrentTerritoryObjective() != nullptr);
    EXPECT_EQ(objectives.GetCurrentTerritoryObjective()->territoryOrder, 1);

    objectives.ResetTerritoryForRound(2);
    const CaptureZone* resetBeach = objectives.GetObjective(1);
    const CaptureZone* resetVilla = objectives.GetObjective(2);
    const CaptureZone* resetFarm = objectives.GetObjective(3);
    const CaptureZone* resetAuxiliary = objectives.GetObjective(4);
    ASSERT_TRUE(resetBeach != nullptr);
    ASSERT_TRUE(resetVilla != nullptr);
    ASSERT_TRUE(resetFarm != nullptr);
    ASSERT_TRUE(resetAuxiliary != nullptr);
    EXPECT_TRUE(resetBeach->isActive);
    EXPECT_EQ(resetBeach->state, CaptureState::Controlled);
    EXPECT_FLOAT_EQ(resetBeach->captureProgress, 0.0f);
    EXPECT_EQ(resetBeach->controllingTeam, 2u);
    EXPECT_FALSE(resetVilla->isActive);
    EXPECT_FALSE(resetFarm->isActive);
    EXPECT_EQ(resetVilla->state, CaptureState::Locked);
    EXPECT_EQ(resetFarm->state, CaptureState::Locked);
    EXPECT_EQ(resetVilla->controllingTeam, 2u);
    EXPECT_EQ(resetFarm->controllingTeam, 2u);
    EXPECT_FALSE(resetAuxiliary->isActive);
    EXPECT_EQ(resetAuxiliary->state, CaptureState::Locked);
    ASSERT_TRUE(objectives.GetCurrentTerritoryObjective() != nullptr);
    EXPECT_EQ(objectives.GetCurrentTerritoryObjective()->id, 1u);

    // Advancing the defender-owned next phase must expose controlled points,
    // not contradictory neutral-state/defender-owner pairs. An auxiliary zone
    // absent from the authored order stays inactive and cannot block victory.
    CaptureZone* secondBeach = objectives.GetObjective(1);
    CaptureZone* secondVilla = objectives.GetObjective(2);
    CaptureZone* secondFarm = objectives.GetObjective(3);
    ASSERT_TRUE(secondBeach != nullptr);
    ASSERT_TRUE(secondVilla != nullptr);
    ASSERT_TRUE(secondFarm != nullptr);
    secondBeach->controllingTeam = 1;
    objectives.ActivateNextTerritory(1);
    EXPECT_TRUE(secondVilla->isActive);
    EXPECT_TRUE(secondFarm->isActive);
    EXPECT_EQ(secondVilla->state, CaptureState::Controlled);
    EXPECT_EQ(secondFarm->state, CaptureState::Controlled);
    secondVilla->controllingTeam = 1;
    secondFarm->controllingTeam = 1;
    EXPECT_TRUE(objectives.AreAllObjectivesCapturedBy(1));
}

TEST(ObjectiveRuntime, EqualMixedOccupancyContestsNeutralPointWithoutAdvance) {
    CaptureZone zone;
    zone.controllingTeam = 0;
    zone.captureProgress = 0.35f;
    zone.cappingTeam = TeamMapping::ServerToRetail(TeamMapping::kServerUs);
    zone.captureSpeed = 0.10f;

    const uint32_t capturedBy = ObjectiveSystem::ProcessNeutralCapture(
        zone, /*team1Occupants=*/2, /*team2Occupants=*/2, 1.0f);

    EXPECT_EQ(capturedBy, 0u);
    EXPECT_EQ(zone.state, CaptureState::Contested);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.35f);
    EXPECT_EQ(zone.cappingTeam,
              TeamMapping::ServerToRetail(TeamMapping::kServerUs));
}

TEST(ObjectiveRuntime, NeutralPointAdvancesOnlyNumericallySuperiorTeam) {
    CaptureZone zone;
    zone.controllingTeam = 0;
    zone.captureSpeed = 0.10f;

    const uint32_t capturedBy = ObjectiveSystem::ProcessNeutralCapture(
        zone, /*team1Occupants=*/1, /*team2Occupants=*/3, 1.0f);

    EXPECT_EQ(capturedBy, 0u);
    EXPECT_EQ(zone.state, CaptureState::Capturing);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.15f);
    EXPECT_EQ(zone.cappingTeam,
              TeamMapping::ServerToRetail(TeamMapping::kServerNva));
}

TEST(ObjectiveRuntime, OpposingTeamNeutralizesPartialProgressBeforeAdvancing) {
    CaptureZone zone;
    zone.controllingTeam = 0;
    zone.captureSpeed = 0.10f;
    zone.captureProgress = 0.30f;
    zone.cappingTeam = TeamMapping::ServerToRetail(TeamMapping::kServerUs);

    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, /*team1Occupants=*/1, /*team2Occupants=*/3, 1.0f),
              0u);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.15f);
    EXPECT_EQ(zone.cappingTeam,
              TeamMapping::ServerToRetail(TeamMapping::kServerUs));

    // The next loaded tick reaches neutral, changes the prospective capper,
    // and carries the remaining elapsed time into North's bar.
    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, /*team1Occupants=*/1, /*team2Occupants=*/3, 2.0f),
              0u);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.15f);
    EXPECT_EQ(zone.cappingTeam,
              TeamMapping::ServerToRetail(TeamMapping::kServerNva));

    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, /*team1Occupants=*/1, /*team2Occupants=*/3, 1.0f),
              0u);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.30f);
    EXPECT_EQ(zone.cappingTeam,
              TeamMapping::ServerToRetail(TeamMapping::kServerNva));
}

TEST(ObjectiveRuntime, NeutralReversalIsInvariantToTickPartitioning) {
    CaptureZone oneTick;
    oneTick.controllingTeam = 0;
    oneTick.captureSpeed = 0.10f;
    oneTick.captureProgress = 0.15f;
    oneTick.cappingTeam = TeamMapping::kRetailUs;
    CaptureZone splitTicks = oneTick;

    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  oneTick, /*team1Occupants=*/1, /*team2Occupants=*/3, 2.0f),
              0u);
    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  splitTicks, /*team1Occupants=*/1, /*team2Occupants=*/3, 1.0f),
              0u);
    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  splitTicks, /*team1Occupants=*/1, /*team2Occupants=*/3, 1.0f),
              0u);

    EXPECT_FLOAT_EQ(oneTick.captureProgress, splitTicks.captureProgress);
    EXPECT_EQ(oneTick.cappingTeam, splitTicks.cappingTeam);
    EXPECT_FLOAT_EQ(oneTick.captureProgress, 0.15f);
    EXPECT_EQ(oneTick.cappingTeam, TeamMapping::kRetailNva);
}

TEST(ObjectiveRuntime, RoundResetRestoresAuthoredOwnersAndClearsPartialCapture) {
    ObjectiveSystem objectives(nullptr);
    objectives.Initialize();

    CaptureZone southHome;
    southHome.id = 1;
    southHome.controllingTeam = TeamMapping::kServerUs;
    southHome.state = CaptureState::Controlled;
    CaptureZone neutral;
    neutral.id = 2;

    objectives.AddObjective(southHome);
    objectives.AddObjective(neutral);
    CaptureZone* changedHome = objectives.GetObjective(1);
    CaptureZone* changedNeutral = objectives.GetObjective(2);
    ASSERT_TRUE(changedHome != nullptr);
    ASSERT_TRUE(changedNeutral != nullptr);
    changedHome->controllingTeam = TeamMapping::kServerNva;
    changedHome->captureProgress = 0.4f;
    changedHome->cappingTeam = TeamMapping::kRetailNva;
    changedNeutral->controllingTeam = TeamMapping::kServerNva;
    changedNeutral->state = CaptureState::Controlled;
    changedNeutral->captureProgress = 0.7f;
    changedNeutral->cappingTeam = TeamMapping::kRetailNva;

    objectives.ResetObjectivesToInitialOwners();

    EXPECT_EQ(changedHome->controllingTeam, TeamMapping::kServerUs);
    EXPECT_EQ(changedHome->state, CaptureState::Controlled);
    EXPECT_FLOAT_EQ(changedHome->captureProgress, 0.0f);
    EXPECT_EQ(changedHome->cappingTeam, uint8_t{0xFF});
    EXPECT_EQ(changedNeutral->controllingTeam, 0u);
    EXPECT_EQ(changedNeutral->state, CaptureState::Neutral);
    EXPECT_FLOAT_EQ(changedNeutral->captureProgress, 0.0f);
    EXPECT_EQ(changedNeutral->cappingTeam, uint8_t{0xFF});
}

TEST(ObjectiveRuntime, DisabledObjectivesStayOutsideActiveGameplayViews) {
    ObjectiveSystem objectives(nullptr);
    objectives.Initialize();

    CaptureZone enabled;
    enabled.id = 1;
    enabled.name = "Enabled";
    enabled.controllingTeam = TeamMapping::kServerUs;

    CaptureZone disabled;
    disabled.id = 2;
    disabled.name = "Disabled";
    disabled.enabled = false;
    disabled.isActive = true;
    disabled.state = CaptureState::Capturing;
    disabled.captureProgress = 0.75f;
    disabled.controllingTeam = TeamMapping::kServerUs;

    objectives.AddObjective(enabled);
    objectives.AddObjective(disabled);

    ASSERT_EQ(objectives.GetObjectiveCount(), 1u);
    EXPECT_EQ(objectives.GetTeamObjectiveCount(TeamMapping::kServerUs), 1u);
    ASSERT_EQ(objectives.GetActiveObjectives().size(), 1u);
    EXPECT_EQ(objectives.GetActiveObjectives().front()->id, 1u);

    CaptureZone* disabledRuntime = objectives.GetObjective(2);
    ASSERT_TRUE(disabledRuntime != nullptr);
    EXPECT_FALSE(disabledRuntime->isActive);
    EXPECT_EQ(disabledRuntime->state, CaptureState::Locked);
    EXPECT_FLOAT_EQ(disabledRuntime->captureProgress, 0.0f);

    // Even a stray writer setting isActive cannot expose a disabled cooked
    // objective through the gameplay-facing active view. The next authority
    // update immediately restores its hard inactive state; round reset retains
    // the same invariant.
    disabledRuntime->isActive = true;
    disabledRuntime->state = CaptureState::Capturing;
    disabledRuntime->captureProgress = 0.75f;
    EXPECT_EQ(objectives.GetActiveObjectives().size(), 1u);
    objectives.Update(1.0f);
    EXPECT_FALSE(disabledRuntime->isActive);
    EXPECT_EQ(disabledRuntime->state, CaptureState::Locked);
    EXPECT_FLOAT_EQ(disabledRuntime->captureProgress, 0.0f);
    objectives.ResetObjectivesToInitialOwners();
    EXPECT_FALSE(disabledRuntime->isActive);
    EXPECT_EQ(disabledRuntime->state, CaptureState::Locked);
}

TEST(ObjectiveRuntime, CaptureDeltaStopsAtNativePhaseBoundary) {
    EXPECT_FLOAT_EQ(ObjectiveSystem::ClampCaptureDeltaToPhase(0.10f, 0.025f),
                    0.025f);
    EXPECT_FLOAT_EQ(ObjectiveSystem::ClampCaptureDeltaToPhase(0.01f, 0.025f),
                    0.01f);
    EXPECT_FLOAT_EQ(ObjectiveSystem::ClampCaptureDeltaToPhase(0.10f, 0.0f),
                    0.0f);
    EXPECT_FLOAT_EQ(ObjectiveSystem::ClampCaptureDeltaToPhase(-1.0f, 1.0f),
                    0.0f);
    EXPECT_FLOAT_EQ(ObjectiveSystem::ClampCaptureDeltaToPhase(
                        std::numeric_limits<float>::infinity(), 1.0f),
                    0.0f);
}

TEST_F(MapRuntimeTest, PerMapRetailObjectivesOverrideGlobalFallback) {
    constexpr std::string_view mapId = "VNTE-Resort";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-Resort.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    WriteRetailPackage(mapAsset);

    WriteText(m_mapsDir / "global_objectives.txt",
              "Alpha -300 0 0 35 0\n"
              "Bravo -100 0 0 35 1\n"
              "Charlie 100 0 0 35 2\n"
              "Delta 300 0 0 35 3\n");
    WriteText(m_mapsDir / std::string(mapId) / "objectives.txt",
              "Beach -11950.550 5238.709 -544.1218 35 0 0 0 0 1 0\n"
              "Temple -3587.457 7545.831 -80.9672 35 1 0 1 1 1 0\n"
              "Villa 1517.340 3289.679 59.9254 35 2 0 2 2 1 0\n"
              "Farm 1262.218 13525.040 -20.8450 35 2 0 3 3 1 0\n"
              "Hotel 8320.872 8080.492 607.2889 35 3 0 4 4 1 0\n");
    WriteText(rotationFile,
              "[VNTE-Resort]\n"
              "display_name=Resort\n"
              "file=" + mapAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto& zones = manager.GetObjectiveZones();
    ASSERT_EQ(zones.size(), 5u) << "per-map Resort data must win over four global fallbacks";

    const std::string expectedNames[] = {"Beach", "Temple", "Villa", "Farm", "Hotel"};
    const int expectedPhases[] = {0, 1, 2, 2, 3};
    std::set<uint8_t> slots;
    std::set<uint8_t> repIndices;
    for (size_t i = 0; i < zones.size(); ++i) {
        EXPECT_EQ(zones[i].name, expectedNames[i]);
        EXPECT_EQ(zones[i].clientSlot, static_cast<uint8_t>(i));
        EXPECT_EQ(zones[i].cookedRepIndex, static_cast<uint8_t>(i));
        EXPECT_EQ(zones[i].territoryOrder, expectedPhases[i]);
        EXPECT_TRUE(zones[i].enabled);
        EXPECT_FALSE(zones[i].connectedToBase);
        EXPECT_EQ(zones[i].cappingTeam, uint8_t{0xFF});
        EXPECT_FALSE(zones[i].lockdownMetadataKnown);
        EXPECT_FALSE(zones[i].lockdownEnabled);
        slots.insert(zones[i].clientSlot);
        repIndices.insert(zones[i].cookedRepIndex);
    }
    EXPECT_EQ(slots.size(), 5u);
    EXPECT_EQ(repIndices.size(), 5u);

    EXPECT_FLOAT_EQ(zones[0].position.x, -11950.550f);
    EXPECT_FLOAT_EQ(zones[2].position.y, 3289.679f);
    EXPECT_FLOAT_EQ(zones[4].position.z, 607.2889f);
}

TEST_F(MapRuntimeTest, PerMapObjectiveLockdownMetadataLoadsTransactionally) {
    constexpr std::string_view mapId = "VNTE-LockdownMetadata";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-LockdownMetadata.roe");

    WriteRetailPackage(mapAsset);
    WriteText(m_mapsDir / std::string(mapId) / "objectives.txt",
              "Alpha 0 0 0 35 0 0 0 10\n"
              "Bravo 100 0 0 35 1 0 1 11\n");
    WriteText(m_mapsDir / std::string(mapId) / "objective_lockdown.txt",
              "# slot enabled time16 time32 time64\n"
              "0 1 500 400 300\n"
              "1 0 240 180 120\n");

    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto& zones = manager.GetObjectiveZones();
    ASSERT_EQ(zones.size(), 2u);
    EXPECT_TRUE(zones[0].lockdownMetadataKnown);
    EXPECT_TRUE(zones[0].lockdownEnabled);
    EXPECT_EQ(zones[0].lockdownTimeSecondsByPlayerBand,
              (std::array<int32_t, 3>{{500, 400, 300}}));
    EXPECT_TRUE(zones[1].lockdownMetadataKnown);
    EXPECT_FALSE(zones[1].lockdownEnabled);
    EXPECT_EQ(zones[1].lockdownTimeSecondsByPlayerBand,
              (std::array<int32_t, 3>{{240, 180, 120}}));
}

TEST_F(MapRuntimeTest, InvalidObjectiveLockdownMetadataFailsClosed) {
    constexpr std::string_view mapId = "VNTE-BadLockdownMetadata";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-BadLockdownMetadata.roe");
    const std::filesystem::path metadata =
        m_mapsDir / std::string(mapId) / "objective_lockdown.txt";

    WriteRetailPackage(mapAsset);
    WriteText(m_mapsDir / std::string(mapId) / "objectives.txt",
              "Alpha 0 0 0 35 0 0 0 10\n"
              "Bravo 100 0 0 35 1 0 1 11\n");
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);
    MapManager manager(nullptr, mapConfig);

    const auto expectRejected = [&](std::string_view contents) {
        WriteText(metadata, contents);
        ASSERT_TRUE(manager.LoadMap(std::string(mapId)));
        const auto& zones = manager.GetObjectiveZones();
        ASSERT_EQ(zones.size(), 2u);
        for (const CaptureZone& zone : zones) {
            EXPECT_FALSE(zone.lockdownMetadataKnown);
            EXPECT_FALSE(zone.lockdownEnabled);
            EXPECT_EQ(zone.lockdownTimeSecondsByPlayerBand,
                      (std::array<int32_t, 3>{{0, 0, 0}}));
        }
    };

    expectRejected("0 1 500 500 500\n1 1 240 240 240 trailing\n");
    expectRejected("0 1 500 500 500\n0 0 240 240 240\n");
    expectRejected("0 1 500 500 500\n2 1 240 240 240\n");
    expectRejected("0 1 500 500 86401\n");
}

TEST_F(MapRuntimeTest, PerMapBotNavigationMetadataLoadsAndRoutesTransactionally) {
    constexpr std::string_view mapId = "VNTE-BotNavigation";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-BotNavigation.roe");

    WriteRetailPackage(mapAsset);
    WriteText(m_mapsDir / std::string(mapId) / "bot_navigation.txt",
              "version 1\n"
              "mode Territories\n"
              "config 6 2 0 0\n"
              "node 1 0 0 0\n"
              "node 2 5 0 0\n"
              "node 3 10 0 0\n"
              "edge 1 2 1\n"
              "edge 2 3 1\n");

    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);
    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const MapBotNavigationMetadata& metadata =
        manager.GetBotNavigationMetadata();
    ASSERT_TRUE(metadata.loaded);
    EXPECT_EQ(metadata.mode, "Territories");
    EXPECT_FALSE(metadata.allowDirectFallback);
    ASSERT_EQ(metadata.graph.nodes.size(), 3u);
    ASSERT_EQ(metadata.graph.edges.size(), 2u);
    EXPECT_FLOAT_EQ(metadata.config.maxEdgeLength, 6.0f);
    EXPECT_FLOAT_EQ(metadata.config.maxEndpointSnapDistance, 2.0f);
    EXPECT_FLOAT_EQ(metadata.config.maxDirectRouteLength, 0.0f);

    BotNavigation navigation;
    ASSERT_TRUE(navigation.Configure(metadata.config));
    ASSERT_TRUE(navigation.ReplaceGraph(metadata.graph));
    const BotRoute route = navigation.BuildRoute(
        Vector3(-1.0f, 0.0f, 0.0f), Vector3(11.0f, 0.0f, 0.0f), false);
    ASSERT_TRUE(route.IsValid());
    EXPECT_TRUE(route.usedAuthoredGraph);
    EXPECT_FALSE(route.usedDirectFallback);
    ASSERT_EQ(route.waypoints.size(), 4u);
    EXPECT_EQ(route.waypoints[0], Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(route.waypoints[2], Vector3(10.0f, 0.0f, 0.0f));
    EXPECT_EQ(route.waypoints[3], Vector3(11.0f, 0.0f, 0.0f));
}

TEST_F(MapRuntimeTest, InvalidBotNavigationMetadataClearsPreviousGraphAndKeepsMapUsable) {
    constexpr std::string_view mapId = "VNTE-BadBotNavigation";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-BadBotNavigation.roe");
    const std::filesystem::path metadataPath =
        m_mapsDir / std::string(mapId) / "bot_navigation.txt";

    WriteRetailPackage(mapAsset);
    WriteText(metadataPath,
              "version 1\n"
              "mode Territories\n"
              "config 10 2 0 0\n"
              "node 1 0 0 0\n"
              "node 2 5 0 0\n"
              "edge 1 2 1\n");
    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);
    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));
    ASSERT_TRUE(manager.GetBotNavigationMetadata().loaded);

    // The reverse arc already belongs to the bidirectional row. A second row
    // would make file order observable, so the entire replacement is rejected.
    WriteText(metadataPath,
              "version 1\n"
              "mode Territories\n"
              "config 10 2 0 0\n"
              "node 1 0 0 0\n"
              "node 2 5 0 0\n"
              "edge 1 2 1\n"
              "edge 2 1 0\n");
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));
    const MapBotNavigationMetadata& rejected =
        manager.GetBotNavigationMetadata();
    EXPECT_FALSE(rejected.loaded);
    EXPECT_TRUE(rejected.graph.IsEmpty());
    EXPECT_EQ(manager.GetCurrentMapName(), std::string(mapId));
}

TEST_F(MapRuntimeTest, InvalidOrDuplicateRetailObjectiveMappingsRemainUnmapped) {
    constexpr std::string_view mapId = "VNTE-MappingValidation";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-MappingValidation.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    WriteRetailPackage(mapAsset);

    WriteText(m_mapsDir / std::string(mapId) / "objectives.txt",
              "GoodA 0 0 0 35 0 0 0 10 1 1\n"
              "BadSlot 10 0 0 35 1 0 16 11\n"
              "BadRep 20 0 0 35 2 0 1 255\n"
              "DupSlot 30 0 0 35 3 0 0 12\n"
              "DupRep 40 0 0 35 4 0 2 10\n"
              "GoodB 50 0 0 35 5 0 2 12 0 0\n"
              "GoodMax 60 0 0 35 6 0 15 254\n");
    WriteText(rotationFile,
              "[VNTE-MappingValidation]\n"
              "display_name=Mapping Validation\n"
              "file=" + mapAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", (m_root / "server_data").generic_string());
    configManager->SetString("DataPaths.maps_path", m_mapsDir.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto& zones = manager.GetObjectiveZones();
    ASSERT_EQ(zones.size(), 7u);

    EXPECT_EQ(zones[0].clientSlot, uint8_t{0});
    EXPECT_EQ(zones[0].cookedRepIndex, uint8_t{10});
    EXPECT_TRUE(zones[0].enabled);
    EXPECT_TRUE(zones[0].connectedToBase);

    for (size_t i = 1; i <= 4; ++i) {
        EXPECT_EQ(zones[i].clientSlot, uint8_t{0xFF}) << "zone " << i;
        EXPECT_EQ(zones[i].cookedRepIndex, uint8_t{0xFF}) << "zone " << i;
    }

    EXPECT_EQ(zones[5].clientSlot, uint8_t{2});
    EXPECT_EQ(zones[5].cookedRepIndex, uint8_t{12});
    EXPECT_FALSE(zones[5].enabled);
    EXPECT_FALSE(zones[5].connectedToBase);
    EXPECT_EQ(zones[5].cappingTeam, uint8_t{0xFF});

    EXPECT_EQ(zones[6].clientSlot, uint8_t{15});
    EXPECT_EQ(zones[6].cookedRepIndex, uint8_t{254});
}

TEST_F(MapRuntimeTest, QuotedObjectiveMetadataLoadsInitialOwnerAndCaptureTime) {
    constexpr std::string_view mapId = "VNSU-QuotedMetadata";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNSU-QuotedMetadata.roe");

    WriteRetailPackage(mapAsset);
    WriteText(m_mapsDir / std::string(mapId) / "objectives.txt",
              "\"Command Post\" 1969.750 645.147 30.355 30 0 0 0 1 1 1 1 25 1 1 1,2\n"
              "\"Governor's House\" 1810.980 -2897.732 191.091 35 1 0 2 3 1 0 0 40 2 0 0,1,3,4\n"
              "LegacyName 0 0 0 30 2 0 3 4\n");

    auto mapConfig = MakeMapConfig(mapId, mapAsset);
    ASSERT_TRUE(mapConfig != nullptr);

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto& zones = manager.GetObjectiveZones();
    ASSERT_EQ(zones.size(), 3u);

    EXPECT_EQ(zones[0].name, "Command Post");
    EXPECT_EQ(zones[0].controllingTeam, 1u);
    EXPECT_EQ(zones[0].state, CaptureState::Controlled);
    EXPECT_TRUE(zones[0].connectedToBase);
    EXPECT_FLOAT_EQ(zones[0].captureSpeed, 1.0f / 25.0f);
    EXPECT_EQ(zones[0].supremacyPointValue, uint8_t{1});
    EXPECT_EQ(zones[0].supremacyHomeTeam, uint8_t{1});
    EXPECT_EQ(zones[0].supremacyAdjacentSlots,
              static_cast<uint16_t>((1u << 1u) | (1u << 2u)));

    EXPECT_EQ(zones[1].name, "Governor's House");
    EXPECT_EQ(zones[1].controllingTeam, 0u);
    EXPECT_EQ(zones[1].state, CaptureState::Neutral);
    EXPECT_FALSE(zones[1].connectedToBase);
    EXPECT_FLOAT_EQ(zones[1].captureSpeed, 1.0f / 40.0f);
    EXPECT_EQ(zones[1].supremacyPointValue, uint8_t{2});
    EXPECT_EQ(zones[1].supremacyHomeTeam, uint8_t{0});
    EXPECT_EQ(zones[1].supremacyAdjacentSlots,
              static_cast<uint16_t>((1u << 0u) | (1u << 1u) |
                                    (1u << 3u) | (1u << 4u)));

    EXPECT_EQ(zones[2].name, "LegacyName");
    EXPECT_EQ(zones[2].controllingTeam, 0u);
    EXPECT_EQ(zones[2].state, CaptureState::Neutral);
    EXPECT_FLOAT_EQ(zones[2].captureSpeed, 0.10f);
    EXPECT_EQ(zones[2].supremacyPointValue, uint8_t{1});
    EXPECT_EQ(zones[2].supremacyHomeTeam, uint8_t{0});
    EXPECT_EQ(zones[2].supremacyAdjacentSlots, uint16_t{0});
}

TEST_F(MapRuntimeTest, AuthoredResortSpawnRequiresMovementBeforeBeachCapture) {
    constexpr std::string_view mapId = "VNTE-Resort";
    const std::filesystem::path mapAsset =
        std::filesystem::absolute(m_externalDir / "VNTE-Resort.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    const std::filesystem::path sourceRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::filesystem::path authoredMaps = sourceRoot / "data" / "maps";

    ASSERT_TRUE(std::filesystem::exists(
        authoredMaps / std::string(mapId) / "objectives.txt"));
    ASSERT_TRUE(std::filesystem::exists(
        authoredMaps / std::string(mapId) / "spawns.txt"));
    ASSERT_TRUE(std::filesystem::exists(
        authoredMaps / std::string(mapId) / "objective_lockdown.txt"));

    WriteRetailPackage(mapAsset);
    WriteText(rotationFile,
              "[VNTE-Resort]\n"
              "display_name=Resort\n"
              "file=" + mapAsset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (sourceRoot / "data").generic_string());
    configManager->SetString("DataPaths.maps_path", authoredMaps.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());

    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const CaptureZone* beach = nullptr;
    const CaptureZone* temple = nullptr;
    const CaptureZone* villa = nullptr;
    const CaptureZone* farm = nullptr;
    const CaptureZone* hotel = nullptr;
    for (const auto& zone : manager.GetObjectiveZones()) {
        if (zone.name == "Beach") beach = &zone;
        else if (zone.name == "Temple") temple = &zone;
        else if (zone.name == "Villa") villa = &zone;
        else if (zone.name == "Farm") farm = &zone;
        else if (zone.name == "Hotel") hotel = &zone;
    }
    ASSERT_NE(beach, nullptr);
    ASSERT_NE(temple, nullptr);
    ASSERT_NE(villa, nullptr);
    ASSERT_NE(farm, nullptr);
    ASSERT_NE(hotel, nullptr);

    EXPECT_TRUE(beach->lockdownMetadataKnown);
    EXPECT_TRUE(beach->lockdownEnabled);
    EXPECT_EQ(beach->lockdownTimeSecondsByPlayerBand,
              (std::array<int32_t, 3>{{500, 500, 500}}));
    for (const CaptureZone* zone : {temple, villa, farm}) {
        EXPECT_TRUE(zone->lockdownMetadataKnown);
        EXPECT_TRUE(zone->lockdownEnabled);
        EXPECT_EQ(zone->lockdownTimeSecondsByPlayerBand,
                  (std::array<int32_t, 3>{{240, 240, 240}}));
    }
    EXPECT_TRUE(hotel->lockdownMetadataKnown);
    EXPECT_FALSE(hotel->lockdownEnabled);
    EXPECT_EQ(hotel->lockdownTimeSecondsByPlayerBand,
              (std::array<int32_t, 3>{{500, 500, 500}}));

    std::vector<Vector3> phaseZeroAttackers;
    uint32_t phaseZeroSpawnVolumeRef = 0;
    for (const auto& spawn : manager.GetSpawnPoints()) {
        const bool availableInPhaseZero =
            (spawn.minTerritoryPhase < 0 || spawn.minTerritoryPhase <= 0) &&
            (spawn.maxTerritoryPhase < 0 || spawn.maxTerritoryPhase >= 0);
        if (spawn.teamId == TeamMapping::kServerUs &&
            availableInPhaseZero) {
            phaseZeroAttackers.push_back(spawn.position);
            phaseZeroSpawnVolumeRef = spawn.retailSpawnVolumeRef;
            EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(*beach, spawn.position))
                << "phase-zero US spawn must not auto-occupy Beach";
        }
    }
    ASSERT_EQ(phaseZeroAttackers.size(), 1u);
    EXPECT_NEAR(phaseZeroAttackers.front().x, -92368.33f, 0.01f);
    EXPECT_NEAR(phaseZeroAttackers.front().y, 6117.922f, 0.01f);
    EXPECT_NEAR(phaseZeroAttackers.front().z, -491.0f, 0.01f);
    EXPECT_EQ(phaseZeroSpawnVolumeRef, 301195u);

    // Drive the same outside -> inside -> outside transition used by live
    // ServerMove positions. Only positions actually inside Beach contribute to
    // the occupancy count passed to capture simulation.
    const Vector3 outsideBeach = phaseZeroAttackers.front();
    const Vector3 insideBeach = beach->position;
    EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(*beach, outsideBeach));
    EXPECT_TRUE(ObjectiveSystem::ContainsPoint2D(*beach, insideBeach));

    CaptureZone capture = *beach;
    capture.controllingTeam = 0;
    capture.state = CaptureState::Neutral;
    capture.captureProgress = 0.0f;
    capture.cappingTeam = uint8_t{0xFF};
    capture.captureSpeed = 0.10f;
    capture.decaySpeed = 0.05f;

    const auto tickAt = [&](const std::vector<Vector3>& attackerPositions) {
        std::size_t occupants = 0;
        for (const Vector3& position : attackerPositions) {
            if (ObjectiveSystem::ContainsPoint2D(capture, position)) ++occupants;
        }
        ObjectiveSystem::ProcessNeutralCapture(
            capture, occupants, /*team2Occupants=*/0, 1.0f);
        return occupants;
    };

    EXPECT_EQ(tickAt({outsideBeach}), 0u);
    EXPECT_FLOAT_EQ(capture.captureProgress, 0.0f);

    // The far-away attacker remains irrelevant when another attacker enters.
    EXPECT_EQ(tickAt({outsideBeach, insideBeach}), 1u);
    const float progressWhileInside = capture.captureProgress;
    EXPECT_FLOAT_EQ(progressWhileInside, 0.10f);

    EXPECT_EQ(tickAt({outsideBeach}), 0u);
    EXPECT_LT(capture.captureProgress, progressWhileInside);
    EXPECT_FLOAT_EQ(capture.captureProgress, 0.05f);
}

TEST(ObjectiveRuntime, CaptureRadiusMetersAreConvertedToRetailUnrealUnits) {
    CaptureZone temple;
    temple.position = {-3587.457f, 7545.831f, -80.9672f};
    temple.captureRadius = 35.0f;

    // Live retail ServerMove trace: 892.527 UU from Temple = 17.85m.
    EXPECT_TRUE(ObjectiveSystem::ContainsPoint2D(
        temple, Vector3{-2731.0f, 7797.0f, 73.0f}));
    EXPECT_TRUE(ObjectiveSystem::ContainsPoint2D(
        temple, Vector3{temple.position.x + 1750.0f, temple.position.y, 5000.0f}));
    EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(
        temple, Vector3{temple.position.x + 1750.1f, temple.position.y, temple.position.z}));
    EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(
        temple, Vector3{std::numeric_limits<float>::quiet_NaN(), temple.position.y, 0.0f}));
    temple.captureRadius = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(temple, temple.position));
}

TEST_F(MapRuntimeTest, AuthoredNonResortProfilesExposeCookedSpawnsAndObjectives) {
    struct ExpectedMap {
        const char* id;
        const char* mode;
        uint32_t usVolume;
        uint32_t nvaVolume;
        size_t usRows;
        size_t nvaRows;
        size_t objectiveCount;
        size_t navigationNodeCount;
        std::array<float, 6> captureSeconds;
    };
    constexpr ExpectedMap maps[] = {
        {"VNTE-CuChi", "Territories", 299107u, 299103u, 4u, 6u, 0u, 0u, {}},
        {"VNSU-HueCity", "Supremacy", 289565u, 289566u, 3u, 3u, 6u, 6u,
         {35.0f, 35.0f, 30.0f, 35.0f, 25.0f, 30.0f}},
        {"VNSK-Compound", "Skirmish", 289129u, 289139u, 3u, 3u, 3u, 0u,
         {10.0f, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f}},
    };

    const std::filesystem::path sourceRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::filesystem::path authoredMaps = sourceRoot / "data" / "maps";
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";
    std::string rotation;
    for (const ExpectedMap& expected : maps) {
        const std::filesystem::path asset = std::filesystem::absolute(
            m_externalDir / (std::string(expected.id) + ".roe"));
        WriteRetailPackage(asset);
        rotation += "[" + std::string(expected.id) + "]\n";
        rotation += "display_name=" + std::string(expected.id) + "\n";
        rotation += "file=" + asset.generic_string() + "\n";
        rotation += "default_mode=" + std::string(expected.mode) + "\n";
        rotation += "supported_modes=" + std::string(expected.mode) + "\n";
    }
    WriteText(rotationFile, rotation);

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (sourceRoot / "data").generic_string());
    configManager->SetString("DataPaths.maps_path", authoredMaps.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    for (const ExpectedMap& expected : maps) {
        ASSERT_TRUE(manager.LoadMap(expected.id));
        std::vector<uint32_t> usRefs;
        std::vector<uint32_t> nvaRefs;
        for (const SpawnPoint& spawn : manager.GetSpawnPoints()) {
            ASSERT_NE(spawn.retailSpawnVolumeRef, 0u);
            if (spawn.teamId == TeamMapping::kServerUs) {
                usRefs.push_back(spawn.retailSpawnVolumeRef);
            } else if (spawn.teamId == TeamMapping::kServerNva) {
                nvaRefs.push_back(spawn.retailSpawnVolumeRef);
            }
        }
        ASSERT_EQ(usRefs.size(), expected.usRows);
        ASSERT_EQ(nvaRefs.size(), expected.nvaRows);
        EXPECT_TRUE(std::all_of(usRefs.begin(), usRefs.end(),
                                [&](uint32_t ref) { return ref == expected.usVolume; }));
        EXPECT_TRUE(std::all_of(nvaRefs.begin(), nvaRefs.end(),
                                [&](uint32_t ref) { return ref == expected.nvaVolume; }));

        const MapBotNavigationMetadata& navigation =
            manager.GetBotNavigationMetadata();
        EXPECT_EQ(navigation.graph.nodes.size(), expected.navigationNodeCount)
            << expected.id;
        EXPECT_EQ(navigation.loaded, expected.navigationNodeCount != 0u)
            << expected.id;
        if (navigation.loaded) {
            EXPECT_EQ(navigation.mode, expected.mode) << expected.id;
            EXPECT_FALSE(navigation.allowDirectFallback) << expected.id;
        }

        if (expected.objectiveCount == 0u) continue;
        const auto& zones = manager.GetObjectiveZones();
        ASSERT_EQ(zones.size(), expected.objectiveCount) << expected.id;
        for (size_t slot = 0; slot < zones.size(); ++slot) {
            EXPECT_EQ(zones[slot].clientSlot, static_cast<uint8_t>(slot))
                << expected.id << " slot " << slot;
            EXPECT_EQ(zones[slot].cookedRepIndex,
                      static_cast<uint8_t>(slot + 1u))
                << expected.id << " slot " << slot;
            EXPECT_FLOAT_EQ(zones[slot].captureSpeed,
                            1.0f / expected.captureSeconds[slot])
                << expected.id << " slot " << slot;
        }
    }
}

TEST_F(MapRuntimeTest, AuthoredHill937LoadsCookedTerritoryChainAndPhaseSpawns) {
    constexpr std::string_view mapId = "VNTE-Hill937";
    const std::filesystem::path sourceRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::filesystem::path authoredMaps = sourceRoot / "data" / "maps";
    const std::filesystem::path asset = std::filesystem::absolute(
        m_externalDir / "VNTE-Hill937.roe");
    const std::filesystem::path rotationFile = m_root / "config" / "maps.ini";

    ASSERT_TRUE(std::filesystem::exists(
        authoredMaps / std::string(mapId) / "objectives.txt"));
    ASSERT_TRUE(std::filesystem::exists(
        authoredMaps / std::string(mapId) / "spawns.txt"));
    WriteRetailPackage(asset);
    WriteText(rotationFile,
              "[VNTE-Hill937]\n"
              "display_name=Hill 937\n"
              "file=" + asset.generic_string() + "\n"
              "default_mode=Territories\n"
              "supported_modes=Territories\n");

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory",
                             (sourceRoot / "data").generic_string());
    configManager->SetString("DataPaths.maps_path", authoredMaps.generic_string());
    configManager->SetString("General.map_rotation_file", rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(
        serverConfig, std::vector<std::filesystem::path>{});
    ASSERT_TRUE(mapConfig->Initialize());

    MapManager manager(nullptr, mapConfig);
    ASSERT_TRUE(manager.LoadMap(std::string(mapId)));

    const auto& zones = manager.GetObjectiveZones();
    ASSERT_EQ(zones.size(), 5u);
    constexpr const char* expectedNames[] = {
        "Abandoned Settlement", "Trenchline", "Trenchline 2",
        "Supply Bunkers", "HeadQuarters"
    };
    constexpr int expectedPhases[] = {0, 0, 1, 1, 2};
    constexpr float expectedCaptureSeconds[] = {32.0f, 32.0f, 38.0f, 38.0f, 32.0f};
    for (std::size_t i = 0; i < zones.size(); ++i) {
        EXPECT_EQ(zones[i].name, expectedNames[i]);
        EXPECT_EQ(zones[i].territoryOrder, expectedPhases[i]);
        EXPECT_EQ(zones[i].controllingTeam, TeamMapping::kServerNva);
        EXPECT_EQ(zones[i].state, CaptureState::Controlled);
        EXPECT_TRUE(zones[i].connectedToBase);
        EXPECT_FLOAT_EQ(zones[i].captureSpeed, 1.0f / expectedCaptureSeconds[i]);
    }

    struct PhaseCounts { std::size_t us = 0; std::size_t nva = 0; };
    std::array<PhaseCounts, 3> phaseCounts{};
    const auto& spawns = manager.GetSpawnPoints();
    ASSERT_EQ(spawns.size(), 30u);
    for (const SpawnPoint& spawn : spawns) {
        ASSERT_EQ(spawn.minTerritoryPhase, spawn.maxTerritoryPhase);
        ASSERT_GE(spawn.minTerritoryPhase, 0);
        ASSERT_LE(spawn.minTerritoryPhase, 2);
        // The cooked export indices are intentionally not claimed as client
        // PackageMap references. Runtime spawning remains fully positional.
        EXPECT_EQ(spawn.retailSpawnVolumeRef, 0u);
        PhaseCounts& counts = phaseCounts[static_cast<std::size_t>(
            spawn.minTerritoryPhase)];
        if (spawn.teamId == TeamMapping::kServerUs) {
            ++counts.us;
        } else if (spawn.teamId == TeamMapping::kServerNva) {
            ++counts.nva;
        } else {
            FAIL() << "unexpected Hill937 spawn team "
                   << static_cast<unsigned>(spawn.teamId);
        }
    }
    EXPECT_EQ(phaseCounts[0].us, 6u);
    EXPECT_EQ(phaseCounts[0].nva, 3u);
    EXPECT_EQ(phaseCounts[1].us, 6u);
    EXPECT_EQ(phaseCounts[1].nva, 6u);
    EXPECT_EQ(phaseCounts[2].us, 6u);
    EXPECT_EQ(phaseCounts[2].nva, 3u);
}

} // namespace

RS2V_TEST_MAIN()
