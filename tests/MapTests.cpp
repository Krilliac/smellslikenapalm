// tests/MapTests.cpp
// Comprehensive unit tests for MapManager and MapConfig

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

// Include actual headers
#include "Game/MapManager.h"
#include "Config/MapConfig.h"
#include "Utils/Logger.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::StrictMock;

// Mock ConfigManager for MapConfig
class MockConfigManager : public ConfigManager {
public:
    MOCK_METHOD(bool, LoadConfiguration, (const std::string&), (override));
    MOCK_METHOD(std::string, GetString, (const std::string& section, const std::string& key, const std::string& defaultVal), (const, override));
    MOCK_METHOD(std::vector<std::string>, GetArray, (const std::string& section, const std::string& key, char delimiter), (const, override));
};

// Fixture for MapConfig tests
class MapConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Prepare temporary config directory
        std::filesystem::create_directory("test_map_cfg");
    }

    void TearDown() override {
        std::filesystem::remove_all("test_map_cfg");
    }

    void WriteConfig(const std::string& name, const std::string& content) {
        std::ofstream ofs("test_map_cfg/" + name);
        ofs << content;
    }

    std::string Path(const std::string& name) {
        return "test_map_cfg/" + name;
    }
};

// MapConfig validation and parsing
TEST_F(MapConfigTest, LoadValidMapConfig_Succeeds) {
    WriteConfig("maps.ini", R"(
[Maps]
Available=MapA,MapB,MapC

[MapRotation]
Maps=MapA,MapB
)");
    MapConfig cfg;
    ASSERT_TRUE(cfg.Initialize(Path("maps.ini")));
    auto avail = cfg.GetAvailableMaps();
    EXPECT_EQ(avail.size(), 3);
    EXPECT_EQ(avail[0], "MapA");
    auto rot = cfg.GetRotation();
    EXPECT_EQ(rot.size(), 2);
    EXPECT_EQ(rot[1], "MapB");
}

TEST_F(MapConfigTest, MissingSection_FailsInitialization) {
    WriteConfig("maps.ini", R"(
[MapRotation]
Maps=MapA,MapB
)");
    MapConfig cfg;
    EXPECT_FALSE(cfg.Initialize(Path("maps.ini")));
}

TEST_F(MapConfigTest, InvalidRotationEntry_FailsInitialization) {
    WriteConfig("maps.ini", R"(
[Maps]
Available=MapA,MapB

[MapRotation]
Maps=MapA,MapC
)");
    MapConfig cfg;
    EXPECT_FALSE(cfg.Initialize(Path("maps.ini")));
}

TEST_F(MapConfigTest, GetSpawnPoints_ReturnsCorrectVectors) {
    // Simulate spawn points via MapManager
    std::filesystem::create_directory("test_maps");
    std::ofstream ofs("test_maps/VTE-CuChi.spawns");
    ofs << "0 0 0\n10 0 10\n-5 0 5\n";
    ofs.close();

    MapManager mgr;
    mgr.LoadSpawnFile("VTE-CuChi", "test_maps/VTE-CuChi.spawns");
    auto spawns = mgr.GetSpawnPoints("VTE-CuChi");
    EXPECT_EQ(spawns.size(), 3);
    EXPECT_FLOAT_EQ(spawns[1].x, 10.0f);
    EXPECT_FLOAT_EQ(spawns[2].z, 5.0f);
}

TEST_F(MapConfigTest, MapManager_LineOfSight_BlockedAndClear) {
    MapManager mgr;
    // Create simple geometry: a wall from x=0..10 at z=5
    mgr.AddStaticBox({5,0,5}, {10,2,1});
    // LOS from (5,0,0) to (5,0,10) blocked
    EXPECT_FALSE(mgr.LineOfSight({5,0,0}, {5,0,10}));
    // LOS from (20,0,0) to (20,0,10) clear
    EXPECT_TRUE(mgr.LineOfSight({20,0,0}, {20,0,10}));
}

TEST_F(MapConfigTest, LoadNonexistentSpawnFile_NoCrash) {
    MapManager mgr;
    EXPECT_NO_THROW(mgr.LoadSpawnFile("Nonexistent", "no_file.spawns"));
    auto spawns = mgr.GetSpawnPoints("Nonexistent");
    EXPECT_TRUE(spawns.empty());
}

TEST_F(MapConfigTest, GetMapBounds_ReturnsCorrectExtents) {
    MapManager mgr;
    // Add two static boxes defining map bounds
    mgr.AddStaticBox({0,0,0}, {10,1,10});
    mgr.AddStaticBox({20,0,20}, {5,1,5});
    BoundingBox bounds = mgr.GetMapBounds();
    EXPECT_FLOAT_EQ(bounds.min.x, -5.0f);
    EXPECT_FLOAT_EQ(bounds.max.x, 22.5f);
}

TEST_F(MapConfigTest, Performance_LoadManyMaps_Efficient) {
    // Stress test MapConfig with many map entries
    std::ostringstream cfg;
    cfg << "[Maps]\nAvailable=";
    for(int i=0;i<1000;i++){
        cfg<<"Map"<<i;
        if(i<999)cfg<<",";
    }
    cfg<<"\n[MapRotation]\nMaps=Map0,Map1,Map2\n";
    WriteConfig("many.ini", cfg.str());
    MapConfig mc;
    auto start = std::chrono::high_resolution_clock::now();
    ASSERT_TRUE(mc.Initialize(Path("many.ini")));
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(ms, 200);  // Load <200ms
    EXPECT_EQ(mc.GetAvailableMaps().size(), 1000);
}