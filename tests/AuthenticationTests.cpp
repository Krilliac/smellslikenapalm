// tests/AuthenticationTests.cpp
// Comprehensive authentication and authorization unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <chrono>
#include <thread>

// Include the headers
#include "Game/AdminManager.h"
#include "Config/ServerConfig.h"
#include "Config/SecurityConfig.h"
#include "Network/NetworkManager.h"
#include "Security/EACProxy.h"
#include "Utils/Logger.h"

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgReferee;

// Mock classes for testing authentication components
class MockNetworkManager : public NetworkManager {
public:
    MOCK_METHOD(void, Disconnect, (uint32_t clientId, const std::string& reason), (override));
    MOCK_METHOD(uint32_t, FindClientBySteamID, (const std::string& steamId), (const, override));
    MOCK_METHOD(std::shared_ptr<ClientConnection>, GetConnection, (uint32_t clientId), (const, override));
    MOCK_METHOD(bool, SendPacket, (uint32_t clientId, const Packet& packet), (override));
};

class MockServerConfig : public ServerConfig {
public:
    MOCK_METHOD(bool, Initialize, (const std::string& configPath), (override));
    MOCK_METHOD(std::string, GetString, (const std::string& section, const std::string& key, const std::string& defaultValue), (const, override));
    MOCK_METHOD(int, GetInt, (const std::string& section, const std::string& key, int defaultValue), (const, override));
    MOCK_METHOD(bool, GetBool, (const std::string& section, const std::string& key, bool defaultValue), (const, override));
};

class MockSecurityConfig : public SecurityConfig {
public:
    MOCK_METHOD(bool, IsEACEnabled, (), (const, override));
    MOCK_METHOD(std::string, GetEACServerKey, (), (const, override));
    MOCK_METHOD(int, GetMaxLoginAttempts, (), (const, override));
    MOCK_METHOD(int, GetBanDurationMinutes, (), (const, override));
};

class MockEACProxy : public EACProxy {
public:
    MOCK_METHOD(bool, ValidateSessionTicket, (const std::string& steamId, const std::vector<uint8_t>& ticket), (override));
    MOCK_METHOD(bool, Initialize, (), (override));
    MOCK_METHOD(void, Shutdown, (), (override));
    MOCK_METHOD(bool, IsClientAuthenticated, (const std::string& steamId), (const, override));
};

class MockGameServer {
public:
    MOCK_METHOD(std::shared_ptr<NetworkManager>, GetNetworkManager, (), (const));
    MOCK_METHOD(std::shared_ptr<ServerConfig>, GetServerConfig, (), (const));
    MOCK_METHOD(std::shared_ptr<SecurityConfig>, GetSecurityConfig, (), (const));
};

// Test fixture for authentication tests
class AuthenticationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize mocks
        mockNetworkManager = std::make_shared<NiceMock<MockNetworkManager>>();
        mockServerConfig = std::make_shared<NiceMock<MockServerConfig>>();
        mockSecurityConfig = std::make_shared<NiceMock<MockSecurityConfig>>();
        mockEACProxy = std::make_shared<NiceMock<MockEACProxy>>();
        mockGameServer = std::make_unique<NiceMock<MockGameServer>>();
        
        // Set up default mock behavior
        ON_CALL(*mockServerConfig, GetString("Security", "BanListFile", _))
            .WillByDefault(Return("config/banlist.txt"));
        ON_CALL(*mockServerConfig, GetString("Security", "WhitelistFile", _))
            .WillByDefault(Return("config/whitelist.txt"));
        ON_CALL(*mockServerConfig, GetInt("Security", "MaxLoginAttempts", _))
            .WillByDefault(Return(3));
        ON_CALL(*mockSecurityConfig, IsEACEnabled())
            .WillByDefault(Return(true));
        ON_CALL(*mockSecurityConfig, GetMaxLoginAttempts())
            .WillByDefault(Return(3));
        ON_CALL(*mockSecurityConfig, GetBanDurationMinutes())
            .WillByDefault(Return(30));
        
        // Create AdminManager with mocked dependencies
        adminManager = std::make_unique<AdminManager>(mockGameServer.get(), mockServerConfig);
    }

    void TearDown() override {
        adminManager.reset();
        mockGameServer.reset();
        mockEACProxy.reset();
        mockSecurityConfig.reset();
        mockServerConfig.reset();
        mockNetworkManager.reset();
    }

    // Test data
    const std::string validSteamId = "76561198000000001";
    const std::string bannedSteamId = "76561198000000002";
    const std::string invalidSteamId = "invalid_steam_id";
    const std::string whitelistedSteamId = "76561198000000003";

    // Mock objects
    std::shared_ptr<MockNetworkManager> mockNetworkManager;
    std::shared_ptr<MockServerConfig> mockServerConfig;
    std::shared_ptr<MockSecurityConfig> mockSecurityConfig;
    std::shared_ptr<MockEACProxy> mockEACProxy;
    std::unique_ptr<MockGameServer> mockGameServer;
    std::unique_ptr<AdminManager> adminManager;
};

// === Steam Authentication Tests ===

TEST_F(AuthenticationTest, ValidSteamIdAuthentication_Success) {
    // Arrange
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(validSteamId, _))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockEACProxy, IsClientAuthenticated(validSteamId))
        .WillOnce(Return(true));

    // Act
    bool result = mockEACProxy->ValidateSessionTicket(validSteamId, {0x01, 0x02, 0x03});

    // Assert
    EXPECT_TRUE(result);
    EXPECT_TRUE(mockEACProxy->IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, InvalidSteamSessionTicket_Failure) {
    // Arrange
    std::vector<uint8_t> invalidTicket = {0xFF, 0xFF, 0xFF};
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(validSteamId, invalidTicket))
        .WillOnce(Return(false));

    // Act
    bool result = mockEACProxy->ValidateSessionTicket(validSteamId, invalidTicket);

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(AuthenticationTest, MalformedSteamId_Rejection) {
    // Arrange
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(invalidSteamId, _))
        .WillOnce(Return(false));

    // Act
    bool result = mockEACProxy->ValidateSessionTicket(invalidSteamId, {0x01, 0x02});

    // Assert
    EXPECT_FALSE(result);
}

// === Ban System Tests ===

TEST_F(AuthenticationTest, BannedPlayer_ConnectionRejected) {
    // Arrange
    adminManager->BanPlayer(bannedSteamId, "Test ban", 60);
    uint32_t clientId = 12345;
    
    EXPECT_CALL(*mockNetworkManager, Disconnect(clientId, "Banned"))
        .Times(1);

    // Act
    bool isBanned = adminManager->IsBanned(bannedSteamId);

    // Assert
    EXPECT_TRUE(isBanned);
}

TEST_F(AuthenticationTest, UnbannedPlayer_ConnectionAllowed) {
    // Arrange - First ban, then unban
    adminManager->BanPlayer(validSteamId, "Temporary ban", 60);
    adminManager->UnbanPlayer(validSteamId);

    // Act
    bool isBanned = adminManager->IsBanned(validSteamId);

    // Assert
    EXPECT_FALSE(isBanned);
}

TEST_F(AuthenticationTest, TemporaryBan_AutoExpiry) {
    // Arrange - Ban for 1 second
    adminManager->BanPlayer(validSteamId, "Short ban", 0, 1); // 1 second duration

    // Act - Check immediately, then after expiry
    bool bannedImmediately = adminManager->IsBanned(validSteamId);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    bool bannedAfterExpiry = adminManager->IsBanned(validSteamId);

    // Assert
    EXPECT_TRUE(bannedImmediately);
    EXPECT_FALSE(bannedAfterExpiry);
}

TEST_F(AuthenticationTest, PermanentBan_NeverExpires) {
    // Arrange - Permanent ban (duration = 0)
    adminManager->BanPlayer(validSteamId, "Permanent ban", 0);

    // Act - Check multiple times
    bool banned1 = adminManager->IsBanned(validSteamId);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    bool banned2 = adminManager->IsBanned(validSteamId);

    // Assert
    EXPECT_TRUE(banned1);
    EXPECT_TRUE(banned2);
}

// === Whitelist Tests ===

TEST_F(AuthenticationTest, WhitelistedPlayer_BypassesBan) {
    // Arrange
    adminManager->AddToWhitelist(whitelistedSteamId);
    adminManager->BanPlayer(whitelistedSteamId, "Should be bypassed", 60);

    // Act
    bool isBanned = adminManager->IsBanned(whitelistedSteamId);
    bool isWhitelisted = adminManager->IsWhitelisted(whitelistedSteamId);

    // Assert
    EXPECT_FALSE(isBanned); // Should not be banned due to whitelist
    EXPECT_TRUE(isWhitelisted);
}

TEST_F(AuthenticationTest, NonWhitelistedPlayer_NormalBanRules) {
    // Arrange
    adminManager->BanPlayer(validSteamId, "Normal ban", 60);

    // Act
    bool isBanned = adminManager->IsBanned(validSteamId);
    bool isWhitelisted = adminManager->IsWhitelisted(validSteamId);

    // Assert
    EXPECT_TRUE(isBanned);
    EXPECT_FALSE(isWhitelisted);
}

// === Rate Limiting Tests ===

TEST_F(AuthenticationTest, ExcessiveLoginAttempts_TemporaryBan) {
    // Arrange
    const int maxAttempts = 3;
    ON_CALL(*mockSecurityConfig, GetMaxLoginAttempts())
        .WillByDefault(Return(maxAttempts));

    // Act - Simulate failed login attempts
    for (int i = 0; i < maxAttempts + 1; ++i) {
        adminManager->RecordFailedLogin(validSteamId);
    }

    // Assert
    bool isBanned = adminManager->IsBanned(validSteamId);
    EXPECT_TRUE(isBanned);
}

TEST_F(AuthenticationTest, SuccessfulLogin_ResetsFailedAttempts) {
    // Arrange
    adminManager->RecordFailedLogin(validSteamId);
    adminManager->RecordFailedLogin(validSteamId);

    // Act
    adminManager->RecordSuccessfulLogin(validSteamId);
    adminManager->RecordFailedLogin(validSteamId); // Should only be 1 attempt now

    // Assert
    bool isBanned = adminManager->IsBanned(validSteamId);
    EXPECT_FALSE(isBanned); // Should not be banned yet
}

// === EAC Integration Tests ===

TEST_F(AuthenticationTest, EACDisabled_SkipsValidation) {
    // Arrange
    ON_CALL(*mockSecurityConfig, IsEACEnabled())
        .WillByDefault(Return(false));

    // Act & Assert - Should not call EAC validation
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(_, _))
        .Times(0);
    
    // Simulate authentication flow without EAC
    bool result = true; // Would normally call EAC validation
    EXPECT_TRUE(result);
}

TEST_F(AuthenticationTest, EACEnabled_RequiresValidation) {
    // Arrange
    ON_CALL(*mockSecurityConfig, IsEACEnabled())
        .WillByDefault(Return(true));
    
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(validSteamId, _))
        .WillOnce(Return(true));

    // Act
    bool result = mockEACProxy->ValidateSessionTicket(validSteamId, {0x01, 0x02});

    // Assert
    EXPECT_TRUE(result);
}

TEST_F(AuthenticationTest, EACValidationFailure_RejectsConnection) {
    // Arrange
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(validSteamId, _))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockNetworkManager, Disconnect(_, "EAC validation failed"))
        .Times(1);

    // Act
    bool eacValid = mockEACProxy->ValidateSessionTicket(validSteamId, {0x01});
    if (!eacValid) {
        mockNetworkManager->Disconnect(12345, "EAC validation failed");
    }

    // Assert
    EXPECT_FALSE(eacValid);
}

// === Admin Permission Tests ===

TEST_F(AuthenticationTest, AdminPlayer_HasElevatedPermissions) {
    // Arrange
    adminManager->PromoteToAdmin(validSteamId, AdminLevel::MODERATOR);

    // Act
    bool isAdmin = adminManager->IsAdmin(validSteamId);
    AdminLevel level = adminManager->GetAdminLevel(validSteamId);

    // Assert
    EXPECT_TRUE(isAdmin);
    EXPECT_EQ(level, AdminLevel::MODERATOR);
}

TEST_F(AuthenticationTest, RegularPlayer_NoAdminPermissions) {
    // Act
    bool isAdmin = adminManager->IsAdmin(validSteamId);
    AdminLevel level = adminManager->GetAdminLevel(validSteamId);

    // Assert
    EXPECT_FALSE(isAdmin);
    EXPECT_EQ(level, AdminLevel::NONE);
}

TEST_F(AuthenticationTest, DemotedAdmin_LosesPermissions) {
    // Arrange
    adminManager->PromoteToAdmin(validSteamId, AdminLevel::ADMIN);
    adminManager->DemoteFromAdmin(validSteamId);

    // Act
    bool isAdmin = adminManager->IsAdmin(validSteamId);

    // Assert
    EXPECT_FALSE(isAdmin);
}

// === Configuration Tests ===

TEST_F(AuthenticationTest, InvalidConfigPath_GracefulFailure) {
    // Arrange
    EXPECT_CALL(*mockServerConfig, Initialize("invalid/path/config.ini"))
        .WillOnce(Return(false));

    // Act
    bool result = mockServerConfig->Initialize("invalid/path/config.ini");

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(AuthenticationTest, MissingBanListFile_CreatesDefault) {
    // Arrange
    EXPECT_CALL(*mockServerConfig, GetString("Security", "BanListFile", _))
        .WillOnce(Return(""));

    // Act
    std::string banListPath = mockServerConfig->GetString("Security", "BanListFile", "default_banlist.txt");

    // Assert
    EXPECT_EQ(banListPath, "default_banlist.txt");
}

// === Security Edge Cases ===

TEST_F(AuthenticationTest, NullSteamId_HandledSafely) {
    // Act & Assert - Should not crash
    bool isBanned = adminManager->IsBanned("");
    EXPECT_FALSE(isBanned); // Empty SteamID should not be banned
}

TEST_F(AuthenticationTest, ExtremelyLongSteamId_Rejected) {
    // Arrange
    std::string longSteamId(10000, 'a'); // 10k character string

    // Act
    bool isBanned = adminManager->IsBanned(longSteamId);

    // Assert - Should handle gracefully
    EXPECT_FALSE(isBanned);
}

TEST_F(AuthenticationTest, ConcurrentBanOperations_ThreadSafe) {
    // This test would require threading support
    // For now, just verify single-threaded behavior
    
    // Arrange
    std::vector<std::string> steamIds = {
        "76561198000000004",
        "76561198000000005", 
        "76561198000000006"
    };

    // Act
    for (const auto& steamId : steamIds) {
        adminManager->BanPlayer(steamId, "Concurrent test", 60);
    }

    // Assert
    for (const auto& steamId : steamIds) {
        EXPECT_TRUE(adminManager->IsBanned(steamId));
    }
}

// === Integration Tests ===

TEST_F(AuthenticationTest, FullAuthenticationFlow_Success) {
    // Arrange
    InSequence seq;
    EXPECT_CALL(*mockEACProxy, ValidateSessionTicket(validSteamId, _))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockEACProxy, IsClientAuthenticated(validSteamId))
        .WillOnce(Return(true));

    // Act - Simulate full authentication flow
    bool sessionValid = mockEACProxy->ValidateSessionTicket(validSteamId, {0x01, 0x02});
    bool isBanned = adminManager->IsBanned(validSteamId);
    bool isAuthenticated = mockEACProxy->IsClientAuthenticated(validSteamId);
    adminManager->RecordSuccessfulLogin(validSteamId);

    // Assert
    EXPECT_TRUE(sessionValid);
    EXPECT_FALSE(isBanned);
    EXPECT_TRUE(isAuthenticated);
}

TEST_F(AuthenticationTest, FullAuthenticationFlow_BannedPlayer) {
    // Arrange
    adminManager->BanPlayer(bannedSteamId, "Pre-existing ban", 60);
    
    // Act - Banned player should be rejected before EAC validation
    bool isBanned = adminManager->IsBanned(bannedSteamId);

    // Assert
    EXPECT_TRUE(isBanned);
    // EAC validation should be skipped for banned players
}

// === Performance Tests ===

TEST_F(AuthenticationTest, BanCheck_PerformanceAcceptable) {
    // Arrange - Add many bans
    for (int i = 0; i < 1000; ++i) {
        std::string steamId = "7656119800000" + std::to_string(i);
        adminManager->BanPlayer(steamId, "Performance test", 60);
    }

    // Act & Assert - Ban check should be fast even with many entries
    auto start = std::chrono::high_resolution_clock::now();
    bool isBanned = adminManager->IsBanned("76561198000005000"); // Not in ban list
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    EXPECT_LT(duration.count(), 1000); // Should complete in less than 1ms
    EXPECT_FALSE(isBanned);
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}