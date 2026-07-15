#include "TestFramework.h"

#include "Config/StartupOptions.h"

#include <string>
#include <vector>

TEST(StartupOptions, DefaultsToConfiguredPortAndPrimaryConfig) {
    const StartupOptions options = ParseStartupOptions({});
    EXPECT_TRUE(options.valid);
    EXPECT_EQ(options.configFile, std::string("config/server.ini"));
    EXPECT_TRUE(options.mapName.empty());
    EXPECT_EQ(options.port, static_cast<std::uint16_t>(0));
    EXPECT_EQ(options.eacPort, static_cast<std::uint16_t>(0));
    EXPECT_FALSE(options.help);
    EXPECT_FALSE(options.version);
}

TEST(StartupOptions, ParsesExplicitConfigAndMaximumPort) {
    const StartupOptions options = ParseStartupOptions(
        {"--config", "D:\\servers\\instance.ini", "--port", "65535",
         "--eac-port", "65534"});
    ASSERT_TRUE(options.valid);
    EXPECT_EQ(options.configFile, std::string("D:\\servers\\instance.ini"));
    EXPECT_EQ(options.port, static_cast<std::uint16_t>(65535));
    EXPECT_EQ(options.eacPort, static_cast<std::uint16_t>(65534));
}

TEST(StartupOptions, AcceptsShortFlagsAndInformationalFlags) {
    const StartupOptions options =
        ParseStartupOptions({"-c", "alt.ini", "-p", "7777", "-m",
                             "VNTE-CuChi", "-h", "-v"});
    ASSERT_TRUE(options.valid);
    EXPECT_EQ(options.configFile, std::string("alt.ini"));
    EXPECT_EQ(options.port, static_cast<std::uint16_t>(7777));
    EXPECT_EQ(options.mapName, std::string("VNTE-CuChi"));
    EXPECT_TRUE(options.help);
    EXPECT_TRUE(options.version);
}

TEST(StartupOptions, RejectsMissingOptionValues) {
    StartupOptions options = ParseStartupOptions({"--config"});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("missing value") != std::string::npos);

    options = ParseStartupOptions({"--port"});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("missing value") != std::string::npos);

    options = ParseStartupOptions({"--eac-port"});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("missing value") != std::string::npos);
}

TEST(StartupOptions, RejectsMalformedAndOutOfRangePortsWithoutThrowing) {
    for (const std::string& port :
         {std::string(""), std::string("abc"), std::string("-1"),
          std::string("0"), std::string("65536"), std::string("7777junk"),
          std::string("999999999999999999999999")}) {
        const StartupOptions options =
            ParseStartupOptions({"--port", port});
        EXPECT_FALSE(options.valid);
        EXPECT_TRUE(options.error.find("port") != std::string::npos);

        const StartupOptions eacOptions =
            ParseStartupOptions({"--eac-port", port});
        EXPECT_FALSE(eacOptions.valid);
        EXPECT_TRUE(eacOptions.error.find("port") != std::string::npos);
    }
}

TEST(StartupOptions, RejectsUnknownArgumentsAndEmptyConfigPath) {
    StartupOptions options = ParseStartupOptions({"--listen-everywhere"});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("unknown argument") != std::string::npos);

    options = ParseStartupOptions({"--config", ""});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("config file") != std::string::npos);

    options = ParseStartupOptions({"--map", ""});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("map name") != std::string::npos);

    options = ParseStartupOptions({"--map", "   "});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("map name") != std::string::npos);

    options = ParseStartupOptions({"--map", "VNTE-Resort\nforged-log"});
    EXPECT_FALSE(options.valid);
    EXPECT_TRUE(options.error.find("map name") != std::string::npos);
}

RS2V_TEST_MAIN()
