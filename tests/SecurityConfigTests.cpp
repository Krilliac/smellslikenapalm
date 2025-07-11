// tests/SecurityConfigTests.cpp
// Domain-specific validation tests for SecurityConfig wrapper
//
// Covers:
// 1. EAC enable/disable and key requirements.
// 2. Login attempt limits and ban durations bounds.
// 3. Whitelist and banlist file paths.
// 4. Cross-field rules: strict mode implications.
// 5. Live reload of security settings.
// 6. Edge cases: missing section, invalid values, zero/negative durations.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "Config/ConfigManager.h"
#include "Config/SecurityConfig.h"

using namespace std::string_literals;

static const std::string kTmpDir = "test_security_cfg";
inline std::string Path(const std::string &f) { return kTmpDir + "/" + f; }

void WriteFile(const std::string &name, const std::string &text) {
    std::filesystem::create_directories(kTmpDir);
    std::ofstream out(Path(name));
    out << text;
}

struct SecurityConfigTest : public ::testing::Test {
    void SetUp() override {
        std::filesystem::remove_all(kTmpDir);
    }
    void TearDown() override {
        std::filesystem::remove_all(kTmpDir);
    }
    ConfigManager mgr;
};

// 1. Missing Security section fails initialization
TEST_F(SecurityConfigTest, MissingSection_Fails) {
    WriteFile("sec.ini", R"(
[Server]
Port=7777
)");
    SecurityConfig sec;
    mgr.LoadConfiguration(Path("sec.ini"));
    EXPECT_FALSE(sec.Initialize(&mgr));
}

// 2. EAC enabled without key fails
TEST_F(SecurityConfigTest, EACEnabledNoKey_Fails) {
    WriteFile("sec.ini", R"(
[Security]
EnableEAC=true
EACServerKey=
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    EXPECT_FALSE(sec.Initialize(&mgr));
}

// 3. EAC disabled ignores key
TEST_F(SecurityConfigTest, EACDisabled_IgnoresKey) {
    WriteFile("sec.ini", R"(
[Security]
EnableEAC=false
EACServerKey=secret
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_FALSE(sec.IsEACEnabled());
    EXPECT_TRUE(sec.GetEACServerKey().empty());
}

// 4. Login attempts bounds
TEST_F(SecurityConfigTest, MaxLoginAttempts_Bounds) {
    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=0
BanDurationMinutes=10
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    EXPECT_FALSE(sec.Initialize(&mgr));

    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=100
BanDurationMinutes=10
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_EQ(sec.GetMaxLoginAttempts(), 100);
}

// 5. Ban duration bounds
TEST_F(SecurityConfigTest, BanDuration_Bounds) {
    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=3
BanDurationMinutes=-5
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    EXPECT_FALSE(sec.Initialize(&mgr));

    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=3
BanDurationMinutes=0
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_EQ(sec.GetBanDurationMinutes(), 0);
}

// 6. Whitelist and banlist paths default and override
TEST_F(SecurityConfigTest, FilePaths_DefaultsAndOverrides) {
    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=3
BanDurationMinutes=10
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_EQ(sec.GetBanListFile(), "banlist.txt");
    EXPECT_EQ(sec.GetWhitelistFile(), "whitelist.txt");

    WriteFile("sec.ini", R"(
[Security]
MaxLoginAttempts=3
BanDurationMinutes=10
BanListFile=/custom/ban.txt
WhitelistFile=/custom/white.txt
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_EQ(sec.GetBanListFile(), "/custom/ban.txt");
    EXPECT_EQ(sec.GetWhitelistFile(), "/custom/white.txt");
}

// 7. Strict mode requires ban duration > 0
TEST_F(SecurityConfigTest, StrictModeRequiresBanDuration) {
    WriteFile("sec.ini", R"(
[Security]
EnableStrictMode=true
MaxLoginAttempts=3
BanDurationMinutes=0
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    EXPECT_FALSE(sec.Initialize(&mgr));

    WriteFile("sec.ini", R"(
[Security]
EnableStrictMode=true
MaxLoginAttempts=3
BanDurationMinutes=1
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_TRUE(sec.GetStrictMode());
}

// 8. Live reload updates settings
TEST_F(SecurityConfigTest, LiveReload_UpdatesSettings) {
    WriteFile("sec.ini", R"(
[Security]
EnableEAC=false
MaxLoginAttempts=3
BanDurationMinutes=10
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_FALSE(sec.IsEACEnabled());
    EXPECT_EQ(sec.GetMaxLoginAttempts(), 3);

    // Modify config
    WriteFile("sec.ini", R"(
[Security]
EnableEAC=true
EACServerKey=key123
MaxLoginAttempts=5
BanDurationMinutes=20
)");
    ASSERT_TRUE(mgr.Reload());
    EXPECT_TRUE(sec.Initialize(&mgr));
    EXPECT_TRUE(sec.IsEACEnabled());
    EXPECT_EQ(sec.GetEACServerKey(), "key123");
    EXPECT_EQ(sec.GetMaxLoginAttempts(), 5);
    EXPECT_EQ(sec.GetBanDurationMinutes(), 20);
}

// 9. Edge: extremely large values
TEST_F(SecurityConfigTest, LargeValues_HandledSafely) {
    WriteFile("sec.ini", R"(
[Security]
EnableEAC=true
EACServerKey=supersecretkey
MaxLoginAttempts=1000000
BanDurationMinutes=1000000
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_EQ(sec.GetMaxLoginAttempts(), 1000000);
    EXPECT_EQ(sec.GetBanDurationMinutes(), 1000000);
}

// 10. Missing keys fall back to defaults
TEST_F(SecurityConfigTest, MissingKeys_DefaultFallback) {
    WriteFile("sec.ini", R"(
[Security]
)");
    mgr.LoadConfiguration(Path("sec.ini"));
    SecurityConfig sec;
    ASSERT_TRUE(sec.Initialize(&mgr));
    EXPECT_FALSE(sec.IsEACEnabled());
    EXPECT_EQ(sec.GetEACServerKey(), "");
    EXPECT_EQ(sec.GetMaxLoginAttempts(), 3);
    EXPECT_EQ(sec.GetBanDurationMinutes(), 30);
}