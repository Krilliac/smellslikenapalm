// tests/SecurityTests.cpp
// Comprehensive unit tests for security subsystem
//
// Covers:
// 1. Authentication (Steam/EAC) success and failure.
// 2. Ban and unban workflows.
// 3. Whitelist enforcement.
// 4. Admin authentication and command authorization.
// 5. Password hashing and verification.
// 6. CSRF token generation/validation (if applicable).
// 7. Edge cases: missing credentials, expired bans, concurrent bans.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <chrono>
#include <thread>

#include "Security/AuthManager.h"
#include "Security/EACProxy.h"
#include "Game/AdminManager.h"
#include "Config/SecurityConfig.h"
#include "Utils/PasswordHasher.h"
#include "Utils/TokenManager.h"

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;

// Mock SecurityConfig
class MockSecurityConfig : public SecurityConfig {
public:
    MOCK_METHOD(bool, IsEACEnabled, (), (const, override));
    MOCK_METHOD(std::string, GetEACServerKey, (), (const, override));
    MOCK_METHOD(int, GetMaxLoginAttempts, (), (const, override));
    MOCK_METHOD(int, GetBanDurationMinutes, (), (const, override));
    MOCK_METHOD(std::vector<std::string>, GetWhitelist, (), (const, override));
};

// Fixture
class SecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg = std::make_shared<NiceMock<MockSecurityConfig>>();
        ON_CALL(*cfg, IsEACEnabled()).WillByDefault(Return(true));
        ON_CALL(*cfg, GetEACServerKey()).WillByDefault(Return("server_key"));
        ON_CALL(*cfg, GetMaxLoginAttempts()).WillByDefault(Return(3));
        ON_CALL(*cfg, GetBanDurationMinutes()).WillByDefault(Return(30));
        ON_CALL(*cfg, GetWhitelist()).WillByDefault(Return(std::vector<std::string>{}));

        auth = std::make_unique<AuthManager>(cfg);
        admin = std::make_unique<AdminManager>(nullptr, cfg);
        tokenMgr = std::make_unique<TokenManager>();
    }

    std::shared_ptr<MockSecurityConfig> cfg;
    std::unique_ptr<AuthManager> auth;
    std::unique_ptr<AdminManager> admin;
    std::unique_ptr<TokenManager> tokenMgr;
};

// 1. Steam/EAC authentication success
TEST_F(SecurityTest, SteamAuth_ValidTicket_Succeeds) {
    std::string steamId = "76561198000000001";
    std::vector<uint8_t> ticket{0x01,0x02,0x03,0x04};
    EXPECT_TRUE(auth->ValidateSteamTicket(steamId, ticket));
}

// 2. Steam/EAC authentication failure
TEST_F(SecurityTest, SteamAuth_InvalidTicket_Fails) {
    std::string steamId = "76561198000000001";
    std::vector<uint8_t> badTicket{0xFF,0xFF};
    EXPECT_FALSE(auth->ValidateSteamTicket(steamId, badTicket));
}

// 3. Ban workflow
TEST_F(SecurityTest, BanUnban_Player) {
    std::string player = "76561198000000002";
    admin->BanPlayer(player, "Test ban", 1);  // 1 minute
    EXPECT_TRUE(admin->IsBanned(player));
    admin->UnbanPlayer(player);
    EXPECT_FALSE(admin->IsBanned(player));
}

// 4. Ban expiration
TEST_F(SecurityTest, BanExpires_AfterDuration) {
    std::string player = "76561198000000003";
    admin->BanPlayer(player, "Short ban", 0); // immediate expiry
    EXPECT_TRUE(admin->IsBanned(player));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_FALSE(admin->IsBanned(player));
}

// 5. Whitelist enforcement
TEST_F(SecurityTest, Whitelist_BypassesBan) {
    std::string player = "76561198000000004";
    ON_CALL(*cfg, GetWhitelist()).WillByDefault(Return(std::vector<std::string>{player}));
    admin->BanPlayer(player, "Ban attempt", 10);
    EXPECT_FALSE(admin->IsBanned(player));
}

// 6. Admin authentication
TEST_F(SecurityTest, AdminLogin_PasswordHashing) {
    std::string pwd = "securePass123";
    auto hash = PasswordHasher::Hash(pwd);
    EXPECT_TRUE(PasswordHasher::Verify(pwd, hash));
    EXPECT_FALSE(PasswordHasher::Verify("wrong", hash));
}

// 7. CSRF token generation and validation
TEST_F(SecurityTest, CSRFToken_GenerateAndValidate) {
    auto token = tokenMgr->GenerateToken("user1");
    EXPECT_TRUE(tokenMgr->ValidateToken("user1", token));
    EXPECT_FALSE(tokenMgr->ValidateToken("user2", token));
}

// 8. Missing credentials
TEST_F(SecurityTest, MissingCredentials_AuthFails) {
    EXPECT_FALSE(auth->Authenticate("", ""));
    EXPECT_FALSE(auth->Authenticate("user", ""));
    EXPECT_FALSE(auth->Authenticate("", "pass"));
}

// 9. Excessive login attempts lockout
TEST_F(SecurityTest, LoginAttempts_Lockout) {
    std::string user = "userX";
    for (int i = 0; i < cfg->GetMaxLoginAttempts(); ++i) {
        EXPECT_FALSE(auth->Authenticate(user, "wrong"));
    }
    EXPECT_TRUE(auth->IsLockedOut(user));
}

// 10. Concurrent bans
TEST_F(SecurityTest, ConcurrentBan_Unban_ThreadSafety) {
    std::string player = "76561198000000005";
    auto thr = [&](){
        for (int i=0;i<1000;i++){
            admin->BanPlayer(player,"t",1);
            admin->UnbanPlayer(player);
        }
    };
    std::thread t1(thr), t2(thr);
    t1.join(); t2.join();
    EXPECT_FALSE(admin->IsBanned(player));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}