// tests/AuthenticationTests.cpp
// Authentication & authorization unit tests for the Security subsystem.
//
// These tests are the executable spec for the Security/ authentication classes
// that were specced-but-never-built:
//
//   * Security/EACProxy        — Steam/EAC session-ticket validation + per-client
//                                authentication state (virtual, mockable).
//   * Security/AuthManager     — ties EAC + credentials + tokens together, with
//                                brute-force lockout.
//   * Security/PasswordHasher  — salted PBKDF2-HMAC-SHA256 hash + constant-time
//                                verify (OpenSSL when available, portable
//                                fallback otherwise).
//   * Security/TokenManager    — per-user session / CSRF tokens.
//
// The classes are deliberately self-contained (no GameServer / NetworkManager /
// ConfigManager wiring) so they can be unit-tested in isolation; integrating
// AuthManager into the live login flow is a separate follow-up.

#include "TestFramework.h"
#include "TestMock.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "Security/EACProxy.h"
#include "Security/AuthManager.h"
#include "Security/PasswordHasher.h"
#include "Security/TokenManager.h"

using ::rs2v::_;
using ::rs2v::Return;
using ::rs2v::InSequence;
using ::rs2v::NiceMock;

// A mock EACProxy exercising the virtual surface callers depend on.
class MockEACProxy : public EACProxy {
public:
    MOCK_METHOD(bool, ValidateSessionTicket,
                (const std::string&, const std::vector<uint8_t>&), (override));
    MOCK_METHOD(bool, Initialize, (), (override));
    MOCK_METHOD(void, Shutdown, (), (override));
    MOCK_METHOD(bool, IsClientAuthenticated, (const std::string&), (const, override));
};

class AuthenticationTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        eac = std::make_unique<EACProxy>();
        eac->Initialize();
        // AuthManager with a null SecurityConfig (it is kept only for forward
        // compatibility) and the default 3-attempt lockout policy.
        auth = std::make_unique<AuthManager>(nullptr, 3);
    }

    void TearDown() override {
        auth.reset();
        if (eac) { eac->Shutdown(); eac.reset(); }
    }

    const std::string validSteamId       = "76561198000000001";
    const std::string anotherSteamId     = "76561198000000003";
    const std::string malformedSteamId   = "invalid_steam_id";

    std::unique_ptr<EACProxy>    eac;
    std::unique_ptr<AuthManager> auth;
};

// =========================================================================
// EACProxy — Steam session-ticket validation
// =========================================================================

TEST_F(AuthenticationTest, ValidSteamIdAuthentication_Success) {
    EXPECT_TRUE(eac->ValidateSessionTicket(validSteamId, {0x01, 0x02, 0x03}));
    EXPECT_TRUE(eac->IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, InvalidSteamSessionTicket_Failure) {
    std::vector<uint8_t> invalidTicket = {0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(eac->ValidateSessionTicket(validSteamId, invalidTicket));
    EXPECT_FALSE(eac->IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, EmptyTicket_Rejected) {
    EXPECT_FALSE(eac->ValidateSessionTicket(validSteamId, {}));
}

TEST_F(AuthenticationTest, MalformedSteamId_Rejection) {
    EXPECT_FALSE(eac->ValidateSessionTicket(malformedSteamId, {0x01, 0x02}));
    EXPECT_FALSE(eac->IsClientAuthenticated(malformedSteamId));
}

TEST_F(AuthenticationTest, FailedValidation_ClearsPriorAuthentication) {
    EXPECT_TRUE(eac->ValidateSessionTicket(validSteamId, {0x01}));
    EXPECT_TRUE(eac->IsClientAuthenticated(validSteamId));
    // A subsequent bad ticket revokes the authenticated state.
    EXPECT_FALSE(eac->ValidateSessionTicket(validSteamId, {0xFF, 0xFF}));
    EXPECT_FALSE(eac->IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, Shutdown_ClearsAuthenticatedClients) {
    EXPECT_TRUE(eac->ValidateSessionTicket(validSteamId, {0x01}));
    EXPECT_TRUE(eac->IsRunning());
    eac->Shutdown();
    EXPECT_FALSE(eac->IsRunning());
    EXPECT_FALSE(eac->IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, RemoveClient_DropsAuthentication) {
    EXPECT_TRUE(eac->ValidateSessionTicket(validSteamId, {0x01}));
    eac->RemoveClient(validSteamId);
    EXPECT_FALSE(eac->IsClientAuthenticated(validSteamId));
}

// =========================================================================
// EACProxy — mock-based flow (matches the original test's intent)
// =========================================================================

TEST_F(AuthenticationTest, MockEAC_ValidationSequence) {
    NiceMock<MockEACProxy> mockEac;
    InSequence seq;
    EXPECT_CALL(mockEac, ValidateSessionTicket(validSteamId, _)).WillOnce(Return(true));
    EXPECT_CALL(mockEac, IsClientAuthenticated(validSteamId)).WillOnce(Return(true));

    EXPECT_TRUE(mockEac.ValidateSessionTicket(validSteamId, {0x01, 0x02}));
    EXPECT_TRUE(mockEac.IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, MockEAC_ValidationFailure) {
    NiceMock<MockEACProxy> mockEac;
    EXPECT_CALL(mockEac, ValidateSessionTicket(validSteamId, _)).WillOnce(Return(false));
    EXPECT_FALSE(mockEac.ValidateSessionTicket(validSteamId, {0x01}));
}

// =========================================================================
// AuthManager — Steam ticket delegation
// =========================================================================

TEST_F(AuthenticationTest, AuthManager_ValidSteamTicket_Succeeds) {
    EXPECT_TRUE(auth->ValidateSteamTicket(validSteamId, {0x01, 0x02, 0x03, 0x04}));
    EXPECT_TRUE(auth->GetEACProxy().IsClientAuthenticated(validSteamId));
}

TEST_F(AuthenticationTest, AuthManager_InvalidSteamTicket_Fails) {
    EXPECT_FALSE(auth->ValidateSteamTicket(validSteamId, {0xFF, 0xFF}));
}

// =========================================================================
// AuthManager — local credentials + lockout
// =========================================================================

TEST_F(AuthenticationTest, AuthManager_RegisterAndAuthenticate_Success) {
    ASSERT_TRUE(auth->RegisterUser("admin", "s3cret-pass"));
    EXPECT_TRUE(auth->Authenticate("admin", "s3cret-pass"));
}

TEST_F(AuthenticationTest, AuthManager_WrongPassword_Fails) {
    ASSERT_TRUE(auth->RegisterUser("admin", "s3cret-pass"));
    EXPECT_FALSE(auth->Authenticate("admin", "wrong"));
}

TEST_F(AuthenticationTest, AuthManager_MissingCredentials_Fail) {
    EXPECT_FALSE(auth->Authenticate("", ""));
    EXPECT_FALSE(auth->Authenticate("user", ""));
    EXPECT_FALSE(auth->Authenticate("", "pass"));
}

TEST_F(AuthenticationTest, AuthManager_ExcessiveFailures_LockOut) {
    auth->RegisterUser("user", "correct");
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(auth->Authenticate("user", "wrong"));
    }
    EXPECT_TRUE(auth->IsLockedOut("user"));
    // Even the correct password is refused once locked out.
    EXPECT_FALSE(auth->Authenticate("user", "correct"));
}

TEST_F(AuthenticationTest, AuthManager_UnknownUser_LocksOutAfterAttempts) {
    // Lockout tracking must work even for never-registered users.
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(auth->Authenticate("ghost", "whatever"));
    }
    EXPECT_TRUE(auth->IsLockedOut("ghost"));
}

TEST_F(AuthenticationTest, AuthManager_SuccessResetsFailedAttempts) {
    auth->RegisterUser("user", "correct");
    EXPECT_FALSE(auth->Authenticate("user", "wrong"));
    EXPECT_FALSE(auth->Authenticate("user", "wrong"));
    EXPECT_TRUE(auth->Authenticate("user", "correct"));   // resets counter
    EXPECT_FALSE(auth->IsLockedOut("user"));
    EXPECT_FALSE(auth->Authenticate("user", "wrong"));    // back to 1 attempt
    EXPECT_FALSE(auth->IsLockedOut("user"));
}

TEST_F(AuthenticationTest, AuthManager_ResetLockout_Recovers) {
    for (int i = 0; i < 3; ++i) auth->Authenticate("user", "wrong");
    ASSERT_TRUE(auth->IsLockedOut("user"));
    auth->ResetLockout("user");
    EXPECT_FALSE(auth->IsLockedOut("user"));
}

// =========================================================================
// AuthManager — session tokens
// =========================================================================

TEST_F(AuthenticationTest, AuthManager_SessionToken_RoundTrip) {
    auto token = auth->IssueSessionToken("admin");
    EXPECT_FALSE(token.empty());
    EXPECT_TRUE(auth->ValidateSessionToken("admin", token));
    EXPECT_FALSE(auth->ValidateSessionToken("someone-else", token));
}

// =========================================================================
// PasswordHasher
// =========================================================================

TEST_F(AuthenticationTest, PasswordHasher_HashVerify_Roundtrip) {
    std::string pwd = "securePass123";
    std::string hash = PasswordHasher::Hash(pwd);
    ASSERT_FALSE(hash.empty());
    EXPECT_TRUE(PasswordHasher::Verify(pwd, hash));
    EXPECT_FALSE(PasswordHasher::Verify("wrong", hash));
}

TEST_F(AuthenticationTest, PasswordHasher_SaltedHash_IsUnique) {
    std::string pwd = "samePassword";
    std::string h1 = PasswordHasher::Hash(pwd);
    std::string h2 = PasswordHasher::Hash(pwd);
    EXPECT_NE(h1, h2);                          // distinct random salt each time
    EXPECT_TRUE(PasswordHasher::Verify(pwd, h1));
    EXPECT_TRUE(PasswordHasher::Verify(pwd, h2));
}

TEST_F(AuthenticationTest, PasswordHasher_EncodedFormat_IsSelfDescribing) {
    std::string hash = PasswordHasher::Hash("pw");
    // pbkdf2$<iter>$<b64salt>$<b64key>
    EXPECT_EQ(hash.rfind("pbkdf2$", 0), 0u);
    int dollars = 0;
    for (char c : hash) if (c == '$') ++dollars;
    EXPECT_EQ(dollars, 3);
}

TEST_F(AuthenticationTest, PasswordHasher_MalformedHash_VerifyFails) {
    EXPECT_FALSE(PasswordHasher::Verify("pw", ""));
    EXPECT_FALSE(PasswordHasher::Verify("pw", "not-a-real-hash"));
    EXPECT_FALSE(PasswordHasher::Verify("pw", "pbkdf2$abc$xx$yy"));
}

TEST_F(AuthenticationTest, PasswordHasher_EmptyPassword_Supported) {
    std::string hash = PasswordHasher::Hash("");
    ASSERT_FALSE(hash.empty());
    EXPECT_TRUE(PasswordHasher::Verify("", hash));
    EXPECT_FALSE(PasswordHasher::Verify("x", hash));
}

// =========================================================================
// TokenManager
// =========================================================================

TEST_F(AuthenticationTest, TokenManager_GenerateValidate) {
    TokenManager tm;
    auto token = tm.GenerateToken("user1");
    EXPECT_FALSE(token.empty());
    EXPECT_TRUE(tm.ValidateToken("user1", token));
    EXPECT_FALSE(tm.ValidateToken("user2", token));   // bound to issuing user
}

TEST_F(AuthenticationTest, TokenManager_UnknownToken_Fails) {
    TokenManager tm;
    EXPECT_FALSE(tm.ValidateToken("user1", "not-a-token"));
}

TEST_F(AuthenticationTest, TokenManager_DistinctTokensPerCall) {
    TokenManager tm;
    auto t1 = tm.GenerateToken("user1");
    auto t2 = tm.GenerateToken("user1");
    EXPECT_NE(t1, t2);
    EXPECT_TRUE(tm.ValidateToken("user1", t1));
    EXPECT_TRUE(tm.ValidateToken("user1", t2));
}

TEST_F(AuthenticationTest, TokenManager_Revoke) {
    TokenManager tm;
    auto token = tm.GenerateToken("user1");
    tm.RevokeToken(token);
    EXPECT_FALSE(tm.ValidateToken("user1", token));
}

TEST_F(AuthenticationTest, TokenManager_Expiry) {
    TokenManager tm(std::chrono::seconds(1));
    auto token = tm.GenerateToken("user1");
    EXPECT_TRUE(tm.ValidateToken("user1", token));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_FALSE(tm.ValidateToken("user1", token));
}

// =========================================================================
// Edge cases
// =========================================================================

TEST_F(AuthenticationTest, EmptySteamId_HandledSafely) {
    EXPECT_FALSE(eac->ValidateSessionTicket("", {0x01}));
    EXPECT_FALSE(eac->IsClientAuthenticated(""));
}

TEST_F(AuthenticationTest, ExtremelyLongSteamId_Rejected) {
    std::string longId(10000, '7');
    EXPECT_FALSE(eac->ValidateSessionTicket(longId, {0x01}));
}

// =========================================================================
// Integration
// =========================================================================

TEST_F(AuthenticationTest, FullAuthenticationFlow_Success) {
    // Credential auth + token issuance + steam ticket validation together.
    ASSERT_TRUE(auth->RegisterUser(validSteamId, "pw"));
    EXPECT_TRUE(auth->Authenticate(validSteamId, "pw"));
    EXPECT_TRUE(auth->ValidateSteamTicket(validSteamId, {0x01, 0x02}));

    auto token = auth->IssueSessionToken(validSteamId);
    EXPECT_TRUE(auth->ValidateSessionToken(validSteamId, token));
    EXPECT_TRUE(auth->GetEACProxy().IsClientAuthenticated(validSteamId));
    EXPECT_FALSE(auth->IsLockedOut(validSteamId));
}

RS2V_TEST_MAIN()
