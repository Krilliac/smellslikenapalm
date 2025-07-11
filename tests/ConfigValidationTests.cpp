// tests/ConfigValidationTests.cpp
// Domain-specific validation tests for configuration wrappers
//
// GoogleTest / GoogleMock

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>

// core headers
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Config/NetworkConfig.h"
#include "Config/SecurityConfig.h"
#include "Config/GameConfig.h"
#include "Config/MapConfig.h"

using namespace std::string_literals;

namespace {

const std::string kTmp = "tmp_cfg_validation";
inline std::string path(const std::string& f) { return kTmp + "/" + f; }

void write(const std::string& file, const std::string& text)
{
    std::filesystem::create_directories(kTmp);
    std::ofstream out(path(file));
    out << text;
}

/* -------------------------------------------------------------------------- */
/*                               Test Fixture                                 */
/* -------------------------------------------------------------------------- */
class ConfigValidationTest : public ::testing::Test
{
protected:
    void SetUp() override   { std::filesystem::create_directories(kTmp); }
    void TearDown() override{ std::filesystem::remove_all(kTmp);         }

    ConfigManager mgr;
};

/* -------------------------------------------------------------------------- */
/*                         ServerConfig validation                             */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, ServerConfig_MissingMandatorySection_Fails)
{
    write("srv.ini",
R"(
[Network]
Port=7777
)");

    auto cfg = std::make_unique<ServerConfig>();
    EXPECT_FALSE(cfg->Initialize(path("srv.ini")));
}

TEST_F(ConfigValidationTest, ServerConfig_InvalidPort_Fails)
{
    write("srv.ini",
R"(
[Server]
Port=99999
MaxPlayers=64
TickRate=60
)");

    auto cfg = std::make_unique<ServerConfig>();
    EXPECT_FALSE(cfg->Initialize(path("srv.ini")));
}

TEST_F(ConfigValidationTest, ServerConfig_DefaultsKickIn_WhenOptionalMissing)
{
    write("srv.ini",
R"(
[Server]
Port=7777
MaxPlayers=32
TickRate=60
)");

    auto cfg = std::make_unique<ServerConfig>();
    ASSERT_TRUE(cfg->Initialize(path("srv.ini")));
    EXPECT_EQ(cfg->GetLogLevel(), LogLevel::INFO);          // default
    EXPECT_EQ(cfg->GetServerName(), "smellslikenapalm");    // default
}

/* -------------------------------------------------------------------------- */
/*                         NetworkConfig validation                            */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, NetworkConfig_CompressionThreshold_RangeCheck)
{
    write("net.ini",
R"(
[Network]
CompressionEnabled=true
CompressionThreshold=1.5
)");

    mgr.LoadConfiguration(path("net.ini"));
    NetworkConfig net;
    ASSERT_TRUE(net.Initialize(&mgr));
    // Out-of-range should be clamped to 1.0
    EXPECT_FLOAT_EQ(net.GetCompressionThreshold(), 1.0f);
}

TEST_F(ConfigValidationTest, NetworkConfig_BandwidthMustBePositive)
{
    write("net.ini",
R"(
[Network]
MaxBandwidthMbps = -10
)");

    mgr.LoadConfiguration(path("net.ini"));
    NetworkConfig net;
    EXPECT_FALSE(net.Initialize(&mgr));
}

/* -------------------------------------------------------------------------- */
/*                       SecurityConfig validation                             */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, SecurityConfig_LoginAttempts_LowerBound)
{
    write("sec.ini",
R"(
[Security]
MaxLoginAttempts = 0
)");

    mgr.LoadConfiguration(path("sec.ini"));
    SecurityConfig sec;
    EXPECT_FALSE(sec.Initialize(&mgr));
}

TEST_F(ConfigValidationTest, SecurityConfig_EACToggle_Sanity)
{
    write("sec.ini",
R"(
[Security]
EnableEAC = false
EACKey     = secret
)");

    mgr.LoadConfiguration(path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_FALSE(sec.IsEACEnabled());
    EXPECT_TRUE(sec.GetEACServerKey().empty()); // key ignored when EAC disabled
}

/* -------------------------------------------------------------------------- */
/*                        GameConfig cross-field rules                         */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, GameConfig_FriendlyFireRequiresDamageScale)
{
    write("game.ini",
R"(
[Game]
FriendlyFire = true
)");

    mgr.LoadConfiguration(path("game.ini"));
    GameConfig game;
    EXPECT_FALSE(game.Initialize(&mgr));   // missing FriendlyFireDamageScale
}

TEST_F(ConfigValidationTest, GameConfig_TickRateDrivesRespawnGranularity)
{
    write("server.ini",
R"(
[Server]
Port=7777
MaxPlayers=64
TickRate=30
)");

    write("game.ini",
R"(
[Game]
FriendlyFire=false
RespawnTime=1   ; seconds
)");

    mgr.LoadConfiguration(path("server.ini"));
    mgr.LoadConfiguration(path("game.ini"));

    GameConfig game;
    ASSERT_TRUE(game.Initialize(&mgr));
    EXPECT_EQ(game.GetRespawnIntervalTicks(), 30);  // 1 s * 30 Hz
}

/* -------------------------------------------------------------------------- */
/*                          MapConfig validation                               */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, MapConfig_RotationMustContainValidMaps)
{
    write("maps.ini",
R"(
[MapRotation]
Maps = NonExistingMap,AnotherFake
)");

    MapConfig maps;
    EXPECT_FALSE(maps.Initialize(path("maps.ini")));
}

TEST_F(ConfigValidationTest, MapConfig_ValidRotation_Succeeds)
{
    write("maps.ini",
R"(
[Maps]
Available = VTE-CuChi,VNLTE-Hill937

[MapRotation]
Maps = VTE-CuChi,VNLTE-Hill937
)");

    MapConfig maps;
    ASSERT_TRUE(maps.Initialize(path("maps.ini")));
    EXPECT_EQ(maps.GetRotation().size(), 2);
}

/* -------------------------------------------------------------------------- */
/*                           Reload propagation                                */
/* -------------------------------------------------------------------------- */

TEST_F(ConfigValidationTest, LiveReload_PropagatesToWrappers)
{
    write("srv.ini",
R"(
[Server]
Port=7777
MaxPlayers=32
TickRate=60
)");

    mgr.LoadConfiguration(path("srv.ini"));
    ServerConfig srv;
    ASSERT_TRUE(srv.Initialize(path("srv.ini")));

    // Update file
    write("srv.ini",
R"(
[Server]
Port=7777
MaxPlayers=64
TickRate=60
)");

    ASSERT_TRUE(mgr.Reload());
    EXPECT_EQ(srv.GetMaxPlayers(), 64);
}

} // namespace

// test-runner
int main(int argc,char**argv)
{
    ::testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}