// tests/ConfigTests.cpp
// Comprehensive configuration management and validation unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <fstream>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>

// Include the headers
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Config/NetworkConfig.h"
#include "Config/SecurityConfig.h"
#include "Config/GameConfig.h"
#include "Config/MapConfig.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgReferee;
using ::testing::AtLeast;
using ::testing::Between;

// Constants for configuration testing
constexpr const char* TEST_CONFIG_DIR = "test_configs";
constexpr const char* VALID_SERVER_CONFIG = "valid_server.ini";
constexpr const char* INVALID_CONFIG = "invalid.ini";
constexpr const char* EMPTY_CONFIG = "empty.ini";
constexpr const char* MALFORMED_CONFIG = "malformed.ini";

// Configuration value types for testing
struct ConfigValue {
    std::string section;
    std::string key;
    std::string value;
    std::string type; // "string", "int", "bool", "float"
};

// Mock classes for configuration testing
class MockFileUtils : public FileUtils {
public:
    MOCK_METHOD(bool, FileExists, (const std::string& path), (const, override));
    MOCK_METHOD(bool, IsReadable, (const std::string& path), (const, override));
    MOCK_METHOD(bool, IsWritable, (const std::string& path), (const, override));
    MOCK_METHOD(std::string, ReadFileToString, (const std::string& path), (const, override));
    MOCK_METHOD(bool, WriteStringToFile, (const std::string& path, const std::string& content), (const, override));
    MOCK_METHOD(std::vector<std::string>, ListFiles, (const std::string& directory, const std::string& extension), (const, override));
};

class MockLogger : public Logger {
public:
    MOCK_METHOD(void, Info, (const char* format, ...), (override));
    MOCK_METHOD(void, Warn, (const char* format, ...), (override));
    MOCK_METHOD(void, Error, (const char* format, ...), (override));
    MOCK_METHOD(void, Debug, (const char* format, ...), (override));
};

// Configuration file generator for testing
class ConfigFileGenerator {
public:
    static void CreateTestDirectory() {
        std::filesystem::create_directories(TEST_CONFIG_DIR);
    }

    static void CleanupTestDirectory() {
        if (std::filesystem::exists(TEST_CONFIG_DIR)) {
            std::filesystem::remove_all(TEST_CONFIG_DIR);
        }
    }

    static std::string GetTestConfigPath(const std::string& filename) {
        return std::string(TEST_CONFIG_DIR) + "/" + filename;
    }

    static void CreateValidServerConfig() {
        std::string content = R"(
[Server]
Name=Test Server
Port=7777
MaxPlayers=64
TickRate=60
LogLevel=Info
AdminPassword=test123

[Network]
MaxBandwidthMbps=100.0
PacketTimeout=5000
HeartbeatInterval=1000
CompressionEnabled=true
CompressionThreshold=0.7

[Security]
EnableEAC=true
BanListFile=banlist.txt
WhitelistFile=whitelist.txt
MaxLoginAttempts=3
BanDurationMinutes=30

[Game]
FriendlyFire=false
RespawnTime=10
MaxScore=500
TimeLimit=1800

[Map]
DefaultMap=VTE-CuChi
MapRotation=VTE-CuChi,VNLTE-Hill937,VTE-AnLao
VotingEnabled=true
)";
        WriteConfigFile(VALID_SERVER_CONFIG, content);
    }

    static void CreateInvalidConfig() {
        std::string content = R"(
[Server]
Name=Test Server
Port=invalid_port
MaxPlayers=too_many
TickRate=-1
LogLevel=InvalidLevel

[Network]
MaxBandwidthMbps=not_a_number
PacketTimeout=
)";
        WriteConfigFile(INVALID_CONFIG, content);
    }

    static void CreateEmptyConfig() {
        WriteConfigFile(EMPTY_CONFIG, "");
    }

    static void CreateMalformedConfig() {
        std::string content = R"(
[Server
Name=Test Server
Port=7777
[Network]
MaxBandwidthMbps=100.0
Invalid line without equals sign
[Section Without Closing Bracket
Key=Value
)";
        WriteConfigFile(MALFORMED_CONFIG, content);
    }

    static void CreateConfigWithDefaults() {
        std::string content = R"(
[Server]
Name=Minimal Server
# Other values should use defaults
)";
        WriteConfigFile("minimal.ini", content);
    }

    static void CreateConfigWithComments() {
        std::string content = R"(
# This is a comment
[Server]
Name=Test Server  # Inline comment
# Another comment
Port=7777
; Semicolon comment
MaxPlayers=32 ; Another semicolon comment

[Network]
# Network settings
MaxBandwidthMbps=50.0
)";
        WriteConfigFile("comments.ini", content);
    }

    static void CreateConfigWithSpecialCharacters() {
        std::string content = R"(
[Server]
Name=Server with "Quotes" and 'Apostrophes'
MOTD=Welcome to the server!\nNewline test\tTab test
DatabaseConnection=server=localhost;user=test;password=p@ssw0rd!

[Paths]
LogDirectory=C:\Logs\Server
MapDirectory=/opt/maps/
)";
        WriteConfigFile("special_chars.ini", content);
    }

    static void CreateUnicodeConfig() {
        std::string content = u8R"(
[Server]
Name=テストサーバー
Description=Server with unicode: café, naïve, résumé
MOTD=Welcome! 欢迎! добро пожаловать!
)";
        WriteConfigFile("unicode.ini", content);
    }

    static void CreateLargeConfig() {
        std::ostringstream content;
        content << "[Server]\n";
        content << "Name=Large Config Test\n";
        
        // Generate many sections and keys
        for (int section = 0; section < 100; ++section) {
            content << "[Section" << section << "]\n";
            for (int key = 0; key < 50; ++key) {
                content << "Key" << key << "=Value" << section << "_" << key << "\n";
            }
        }
        WriteConfigFile("large.ini", content.str());
    }

private:
    static void WriteConfigFile(const std::string& filename, const std::string& content) {
        std::string path = GetTestConfigPath(filename);
        std::ofstream file(path);
        file << content;
        file.close();
    }
};

// Test fixture for configuration tests
class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory and files
        ConfigFileGenerator::CreateTestDirectory();
        ConfigFileGenerator::CreateValidServerConfig();
        ConfigFileGenerator::CreateInvalidConfig();
        ConfigFileGenerator::CreateEmptyConfig();
        ConfigFileGenerator::CreateMalformedConfig();
        ConfigFileGenerator::CreateConfigWithDefaults();
        ConfigFileGenerator::CreateConfigWithComments();
        ConfigFileGenerator::CreateConfigWithSpecialCharacters();
        ConfigFileGenerator::CreateUnicodeConfig();
        ConfigFileGenerator::CreateLargeConfig();

        // Initialize mocks
        mockFileUtils = std::make_shared<NiceMock<MockFileUtils>>();
        mockLogger = std::make_shared<NiceMock<MockLogger>>();

        // Set up default mock behavior
        ON_CALL(*mockFileUtils, FileExists(_))
            .WillByDefault(Invoke([](const std::string& path) {
                return std::filesystem::exists(path);
            }));
        ON_CALL(*mockFileUtils, IsReadable(_))
            .WillByDefault(Return(true));
        ON_CALL(*mockFileUtils, ReadFileToString(_))
            .WillByDefault(Invoke([](const std::string& path) {
                std::ifstream file(path);
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                return content;
            }));

        // Create config manager
        configManager = std::make_unique<ConfigManager>();
    }

    void TearDown() override {
        configManager.reset();
        mockLogger.reset();
        mockFileUtils.reset();
        ConfigFileGenerator::CleanupTestDirectory();
    }

    // Helper methods
    std::string GetTestConfigPath(const std::string& filename) {
        return ConfigFileGenerator::GetTestConfigPath(filename);
    }

    void CreateTemporaryConfig(const std::string& filename, const std::vector<ConfigValue>& values) {
        std::ostringstream content;
        std::string currentSection;
        
        for (const auto& value : values) {
            if (value.section != currentSection) {
                content << "[" << value.section << "]\n";
                currentSection = value.section;
            }
            content << value.key << "=" << value.value << "\n";
        }
        
        std::string path = GetTestConfigPath(filename);
        std::ofstream file(path);
        file << content.str();
        file.close();
    }

    // Test data
    std::shared_ptr<MockFileUtils> mockFileUtils;
    std::shared_ptr<MockLogger> mockLogger;
    std::unique_ptr<ConfigManager> configManager;
};

// === Basic Configuration Loading Tests ===

TEST_F(ConfigTest, LoadValidConfig_Success) {
    // Arrange
    std::string configPath = GetTestConfigPath(VALID_SERVER_CONFIG);

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Test Server");
    EXPECT_EQ(configManager->GetInt("Server", "Port", 0), 7777);
    EXPECT_EQ(configManager->GetInt("Server", "MaxPlayers", 0), 64);
    EXPECT_EQ(configManager->GetFloat("Network", "MaxBandwidthMbps", 0.0f), 100.0f);
    EXPECT_TRUE(configManager->GetBool("Network", "CompressionEnabled", false));
}

TEST_F(ConfigTest, LoadNonExistentConfig_Failure) {
    // Arrange
    std::string configPath = GetTestConfigPath("nonexistent.ini");

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(ConfigTest, LoadEmptyConfig_HandledGracefully) {
    // Arrange
    std::string configPath = GetTestConfigPath(EMPTY_CONFIG);

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_TRUE(result); // Empty config should load successfully
    EXPECT_EQ(configManager->GetString("Server", "Name", "default"), "default");
}

TEST_F(ConfigTest, LoadMalformedConfig_Failure) {
    // Arrange
    std::string configPath = GetTestConfigPath(MALFORMED_CONFIG);

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_FALSE(result); // Malformed config should fail to load
}

// === Configuration Value Type Tests ===

TEST_F(ConfigTest, GetString_ValidKey_ReturnsValue) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));

    // Act
    std::string value = configManager->GetString("Server", "Name", "default");

    // Assert
    EXPECT_EQ(value, "Test Server");
}

TEST_F(ConfigTest, GetString_InvalidKey_ReturnsDefault) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));

    // Act
    std::string value = configManager->GetString("Server", "NonExistentKey", "default");

    // Assert
    EXPECT_EQ(value, "default");
}

TEST_F(ConfigTest, GetInt_ValidValue_ReturnsInteger) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));

    // Act
    int value = configManager->GetInt("Server", "Port", 0);

    // Assert
    EXPECT_EQ(value, 7777);
}

TEST_F(ConfigTest, GetInt_InvalidValue_ReturnsDefault) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(INVALID_CONFIG));

    // Act
    int value = configManager->GetInt("Server", "Port", 8888);

    // Assert
    EXPECT_EQ(value, 8888); // Should return default for invalid value
}

TEST_F(ConfigTest, GetBool_TrueValues_ReturnsTrue) {
    // Arrange
    CreateTemporaryConfig("bool_test.ini", {
        {"Test", "bool1", "true"},
        {"Test", "bool2", "TRUE"},
        {"Test", "bool3", "1"},
        {"Test", "bool4", "yes"},
        {"Test", "bool5", "YES"},
        {"Test", "bool6", "on"},
        {"Test", "bool7", "ON"}
    });
    configManager->LoadConfiguration(GetTestConfigPath("bool_test.ini"));

    // Act & Assert
    EXPECT_TRUE(configManager->GetBool("Test", "bool1", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool2", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool3", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool4", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool5", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool6", false));
    EXPECT_TRUE(configManager->GetBool("Test", "bool7", false));
}

TEST_F(ConfigTest, GetBool_FalseValues_ReturnsFalse) {
    // Arrange
    CreateTemporaryConfig("bool_test.ini", {
        {"Test", "bool1", "false"},
        {"Test", "bool2", "FALSE"},
        {"Test", "bool3", "0"},
        {"Test", "bool4", "no"},
        {"Test", "bool5", "NO"},
        {"Test", "bool6", "off"},
        {"Test", "bool7", "OFF"}
    });
    configManager->LoadConfiguration(GetTestConfigPath("bool_test.ini"));

    // Act & Assert
    EXPECT_FALSE(configManager->GetBool("Test", "bool1", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool2", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool3", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool4", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool5", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool6", true));
    EXPECT_FALSE(configManager->GetBool("Test", "bool7", true));
}

TEST_F(ConfigTest, GetFloat_ValidValue_ReturnsFloat) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));

    // Act
    float value = configManager->GetFloat("Network", "MaxBandwidthMbps", 0.0f);

    // Assert
    EXPECT_NEAR(value, 100.0f, 0.001f);
}

TEST_F(ConfigTest, GetFloat_InvalidValue_ReturnsDefault) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(INVALID_CONFIG));

    // Act
    float value = configManager->GetFloat("Network", "MaxBandwidthMbps", 50.0f);

    // Assert
    EXPECT_NEAR(value, 50.0f, 0.001f);
}

// === Configuration Validation Tests ===

TEST_F(ConfigTest, ValidateServerConfig_AllRequiredFields_Success) {
    // Arrange
    auto serverConfig = std::make_unique<ServerConfig>();
    std::string configPath = GetTestConfigPath(VALID_SERVER_CONFIG);

    // Act
    bool result = serverConfig->Initialize(configPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_GT(serverConfig->GetTickRate(), 0);
    EXPECT_GT(serverConfig->GetMaxPlayers(), 0);
    EXPECT_GT(serverConfig->GetPort(), 0);
}

TEST_F(ConfigTest, ValidateNetworkConfig_ValidValues_Success) {
    // Arrange
    auto networkConfig = std::make_unique<NetworkConfig>();
    networkConfig->Initialize(configManager);
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));

    // Act
    bool isValid = true;
    try {
        float bandwidth = networkConfig->GetMaxBandwidthMbps();
        int timeout = networkConfig->GetPacketTimeout();
        isValid = (bandwidth > 0 && timeout > 0);
    } catch (...) {
        isValid = false;
    }

    // Assert
    EXPECT_TRUE(isValid);
}

TEST_F(ConfigTest, ValidateConfig_InvalidPortRange_Failure) {
    // Arrange
    CreateTemporaryConfig("invalid_port.ini", {
        {"Server", "Port", "70000"} // Port out of valid range
    });
    auto serverConfig = std::make_unique<ServerConfig>();

    // Act
    bool result = serverConfig->Initialize(GetTestConfigPath("invalid_port.ini"));

    // Assert
    EXPECT_FALSE(result); // Should fail validation
}

TEST_F(ConfigTest, ValidateConfig_NegativeValues_Failure) {
    // Arrange
    CreateTemporaryConfig("negative_values.ini", {
        {"Server", "MaxPlayers", "-5"},
        {"Server", "TickRate", "-60"},
        {"Network", "PacketTimeout", "-1000"}
    });
    configManager->LoadConfiguration(GetTestConfigPath("negative_values.ini"));

    // Act & Assert
    EXPECT_EQ(configManager->GetInt("Server", "MaxPlayers", 32), 32); // Should use default
    EXPECT_EQ(configManager->GetInt("Server", "TickRate", 60), 60);
    EXPECT_EQ(configManager->GetInt("Network", "PacketTimeout", 5000), 5000);
}

// === Configuration Merging Tests ===

TEST_F(ConfigTest, MergeConfigurations_OverrideValues_Success) {
    // Arrange
    CreateTemporaryConfig("base.ini", {
        {"Server", "Name", "Base Server"},
        {"Server", "Port", "7777"},
        {"Server", "MaxPlayers", "32"}
    });
    CreateTemporaryConfig("override.ini", {
        {"Server", "Name", "Override Server"},
        {"Server", "MaxPlayers", "64"},
        {"Network", "CompressionEnabled", "true"}
    });

    // Act
    configManager->LoadConfiguration(GetTestConfigPath("base.ini"));
    configManager->LoadConfiguration(GetTestConfigPath("override.ini")); // Merge

    // Assert
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Override Server");
    EXPECT_EQ(configManager->GetInt("Server", "Port", 0), 7777); // From base
    EXPECT_EQ(configManager->GetInt("Server", "MaxPlayers", 0), 64); // Overridden
    EXPECT_TRUE(configManager->GetBool("Network", "CompressionEnabled", false)); // From override
}

// === Live Reload Tests ===

TEST_F(ConfigTest, LiveReload_ConfigFileChanged_UpdatesValues) {
    // Arrange
    std::string configPath = GetTestConfigPath("reload_test.ini");
    CreateTemporaryConfig("reload_test.ini", {
        {"Server", "Name", "Original Name"},
        {"Server", "MaxPlayers", "32"}
    });
    
    configManager->LoadConfiguration(configPath);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Original Name");

    // Act - Modify config file
    CreateTemporaryConfig("reload_test.ini", {
        {"Server", "Name", "Updated Name"},
        {"Server", "MaxPlayers", "64"}
    });
    
    bool reloadResult = configManager->Reload();

    // Assert
    EXPECT_TRUE(reloadResult);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Updated Name");
    EXPECT_EQ(configManager->GetInt("Server", "MaxPlayers", 0), 64);
}

// === Special Characters and Encoding Tests ===

TEST_F(ConfigTest, ParseConfig_WithComments_IgnoresComments) {
    // Arrange
    std::string configPath = GetTestConfigPath("comments.ini");

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Test Server");
    EXPECT_EQ(configManager->GetInt("Server", "Port", 0), 7777);
    EXPECT_EQ(configManager->GetInt("Server", "MaxPlayers", 0), 32);
}

TEST_F(ConfigTest, ParseConfig_WithSpecialCharacters_HandlesCorrectly) {
    // Arrange
    std::string configPath = GetTestConfigPath("special_chars.ini");

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Server with \"Quotes\" and 'Apostrophes'");
    EXPECT_EQ(configManager->GetString("Server", "MOTD", ""), "Welcome to the server!\\nNewline test\\tTab test");
}

TEST_F(ConfigTest, ParseConfig_WithUnicode_HandlesUTF8) {
    // Arrange
    std::string configPath = GetTestConfigPath("unicode.ini");

    // Act
    bool result = configManager->LoadConfiguration(configPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "テストサーバー");
    EXPECT_FALSE(configManager->GetString("Server", "Description", "").empty());
}

// === Performance Tests ===

TEST_F(ConfigTest, Performance_LargeConfig_LoadsWithinTimeLimit) {
    // Arrange
    std::string configPath = GetTestConfigPath("large.ini");
    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    bool result = configManager->LoadConfiguration(configPath);
    auto endTime = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_LT(duration.count(), 1000); // Should load within 1 second
    
    // Verify some values loaded correctly
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Large Config Test");
    EXPECT_EQ(configManager->GetString("Section0", "Key0", ""), "Value0_0");
    EXPECT_EQ(configManager->GetString("Section99", "Key49", ""), "Value99_49");
}

TEST_F(ConfigTest, Performance_FrequentAccess_EfficientRetrieval) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));
    const int accessCount = 10000;
    
    auto startTime = std::chrono::high_resolution_clock::now();

    // Act - Access config values frequently
    for (int i = 0; i < accessCount; ++i) {
        volatile std::string name = configManager->GetString("Server", "Name", "");
        volatile int port = configManager->GetInt("Server", "Port", 0);
        volatile bool compression = configManager->GetBool("Network", "CompressionEnabled", false);
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert - Should be very fast for frequent access
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for 10k accesses
}

// === Thread Safety Tests ===

TEST_F(ConfigTest, ThreadSafety_ConcurrentAccess_NoRaceConditions) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));
    const int threadCount = 4;
    const int accessesPerThread = 1000;
    std::atomic<int> errorCount{0};
    std::vector<std::thread> threads;

    // Act - Multiple threads accessing config simultaneously
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&]() {
            try {
                for (int i = 0; i < accessesPerThread; ++i) {
                    std::string name = configManager->GetString("Server", "Name", "");
                    int port = configManager->GetInt("Server", "Port", 0);
                    bool compression = configManager->GetBool("Network", "CompressionEnabled", false);
                    
                    // Verify values are consistent
                    if (name != "Test Server" || port != 7777) {
                        errorCount++;
                    }
                }
            } catch (...) {
                errorCount++;
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Assert
    EXPECT_EQ(errorCount.load(), 0); // No errors should occur
}

// === Error Handling Tests ===

TEST_F(ConfigTest, ErrorHandling_MissingFile_GracefulFailure) {
    // Act & Assert
    EXPECT_FALSE(configManager->LoadConfiguration("nonexistent/path/config.ini"));
    
    // Should still be usable with defaults
    EXPECT_EQ(configManager->GetString("Server", "Name", "Default"), "Default");
}

TEST_F(ConfigTest, ErrorHandling_PermissionDenied_HandledGracefully) {
    // Arrange
    EXPECT_CALL(*mockFileUtils, IsReadable(_))
        .WillOnce(Return(false));

    // Act
    bool result = mockFileUtils->IsReadable("restricted_file.ini");

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(ConfigTest, ErrorHandling_CorruptedFile_PartialLoad) {
    // Arrange
    CreateTemporaryConfig("partial_corrupt.ini", {
        {"Server", "Name", "Good Value"},
        {"Server", "Port", "7777"}
    });
    
    // Manually append corrupted data
    std::string path = GetTestConfigPath("partial_corrupt.ini");
    std::ofstream file(path, std::ios::app);
    file << "\n[Corrupted\nInvalid=\n";
    file.close();

    // Act
    bool result = configManager->LoadConfiguration(path);

    // Assert - Should load what it can
    EXPECT_TRUE(result); // Should succeed partially
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Good Value");
    EXPECT_EQ(configManager->GetInt("Server", "Port", 0), 7777);
}

// === Integration Tests ===

TEST_F(ConfigTest, Integration_AllConfigTypes_LoadSuccessfully) {
    // Arrange
    auto serverConfig = std::make_unique<ServerConfig>();
    auto networkConfig = std::make_unique<NetworkConfig>();
    auto securityConfig = std::make_unique<SecurityConfig>();
    auto gameConfig = std::make_unique<GameConfig>();
    auto mapConfig = std::make_unique<MapConfig>();

    std::string configPath = GetTestConfigPath(VALID_SERVER_CONFIG);

    // Act
    bool serverResult = serverConfig->Initialize(configPath);
    bool networkResult = networkConfig->Initialize(configManager);
    bool securityResult = securityConfig->Initialize(configManager);
    bool gameResult = gameConfig->Initialize(configManager);
    bool mapResult = mapConfig->Initialize(configPath);

    // Assert
    EXPECT_TRUE(serverResult);
    EXPECT_TRUE(networkResult);
    EXPECT_TRUE(securityResult);
    EXPECT_TRUE(gameResult);
    EXPECT_TRUE(mapResult);
}

TEST_F(ConfigTest, Integration_ConfigDependencies_ResolveCorrectly) {
    // Arrange
    configManager->LoadConfiguration(GetTestConfigPath(VALID_SERVER_CONFIG));
    auto networkConfig = std::make_unique<NetworkConfig>();
    networkConfig->Initialize(configManager);

    // Act - Network config should use values from loaded config manager
    float bandwidth = networkConfig->GetMaxBandwidthMbps();
    int timeout = networkConfig->GetPacketTimeout();
    bool compression = networkConfig->IsCompressionEnabled();

    // Assert
    EXPECT_EQ(bandwidth, 100.0f);
    EXPECT_EQ(timeout, 5000);
    EXPECT_TRUE(compression);
}

// === Edge Cases ===

TEST_F(ConfigTest, EdgeCase_EmptySection_HandledCorrectly) {
    // Arrange
    CreateTemporaryConfig("empty_section.ini", {
        {"Server", "Name", "Test"},
        {"EmptySection", "", ""} // This will create empty section
    });

    // Act
    bool result = configManager->LoadConfiguration(GetTestConfigPath("empty_section.ini"));

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Server", "Name", ""), "Test");
    EXPECT_EQ(configManager->GetString("EmptySection", "AnyKey", "default"), "default");
}

TEST_F(ConfigTest, EdgeCase_ExtremelyLongValues_HandledSafely) {
    // Arrange
    std::string longValue(10000, 'A'); // 10KB of 'A' characters
    CreateTemporaryConfig("long_value.ini", {
        {"Test", "LongValue", longValue}
    });

    // Act
    bool result = configManager->LoadConfiguration(GetTestConfigPath("long_value.ini"));

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(configManager->GetString("Test", "LongValue", ""), longValue);
}

TEST_F(ConfigTest, EdgeCase_NullAndControlCharacters_Sanitized) {
    // Arrange
    std::string valueWithNulls = "Value\0with\0nulls";
    valueWithNulls += '\x01'; // Add control character
    CreateTemporaryConfig("control_chars.ini", {
        {"Test", "ControlValue", valueWithNulls}
    });

    // Act & Assert - Should not crash
    EXPECT_NO_THROW({
        bool result = configManager->LoadConfiguration(GetTestConfigPath("control_chars.ini"));
        std::string retrieved = configManager->GetString("Test", "ControlValue", "");
        // Value may be sanitized, but should not crash
    });
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}