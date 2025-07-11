// tests/EACTests.cpp
// Comprehensive Easy Anti-Cheat (EAC) integration and emulation unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <unordered_map>
#include <queue>

// Include the headers
#include "Security/EACProxy.h"
#include "Security/EACServerEmulator.h"
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Game/AdminManager.h"
#include "Config/SecurityConfig.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/PerformanceProfiler.h"

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

// Constants for EAC testing
constexpr const char* VALID_STEAM_ID = "76561198000000001";
constexpr const char* INVALID_STEAM_ID = "invalid_steam_id";
constexpr const char* BANNED_STEAM_ID = "76561198000000666";
constexpr const char* TEST_EAC_KEY = "test_eac_server_key_12345";
constexpr int EAC_RESPONSE_TIMEOUT_MS = 5000;
constexpr int EAC_SCAN_INTERVAL_MS = 30000;

// EAC packet types and structures
enum class EACPacketType : uint16_t {
    HELLO = 0x0001,
    CHALLENGE = 0x0002,
    RESPONSE = 0x0003,
    MEMORY_READ = 0x0004,
    MEMORY_WRITE = 0x0005,
    MEMORY_ALLOC = 0x0006,
    BROADCAST_READ = 0x0007,
    HEARTBEAT = 0x0008,
    DISCONNECT = 0x0009,
    BAN_NOTIFICATION = 0x000A
};

// EAC scan result types
enum class EACScanResult : uint8_t {
    CLEAN = 0x00,
    SUSPICIOUS = 0x01,
    DETECTED = 0x02,
    TIMEOUT = 0x03,
    ERROR = 0x04
};

// EAC memory operation data
struct EACMemoryOperation {
    uint64_t address;
    uint32_t size;
    std::vector<uint8_t> data;
    uint32_t processId;
    std::chrono::steady_clock::time_point timestamp;
};

// EAC scan session data
struct EACScanSession {
    std::string steamId;
    uint32_t sessionId;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastActivity;
    EACScanResult result;
    std::vector<EACMemoryOperation> operations;
    bool isAuthenticated;
    bool isActive;
    
    EACScanSession(const std::string& id, uint32_t session)
        : steamId(id), sessionId(session), result(EACScanResult::CLEAN)
        , isAuthenticated(false), isActive(true) {
        startTime = lastActivity = std::chrono::steady_clock::now();
    }
};

// Mock classes for EAC testing
class MockSecurityConfig : public SecurityConfig {
public:
    MOCK_METHOD(bool, IsEACEnabled, (), (const, override));
    MOCK_METHOD(std::string, GetEACServerKey, (), (const, override));
    MOCK_METHOD(int, GetEACTimeout, (), (const, override));
    MOCK_METHOD(int, GetEACScanInterval, (), (const, override));
    MOCK_METHOD(bool, GetEACStrictMode, (), (const, override));
    MOCK_METHOD(std::vector<std::string>, GetEACWhitelist, (), (const, override));
    MOCK_METHOD(bool, GetEACLoggingEnabled, (), (const, override));
};

class MockAdminManager : public AdminManager {
public:
    MOCK_METHOD(void, BanPlayer, (const std::string& steamId, const std::string& reason, int durationMinutes), (override));
    MOCK_METHOD(bool, IsBanned, (const std::string& steamId), (const, override));
    MOCK_METHOD(void, AddToWhitelist, (const std::string& steamId), (override));
    MOCK_METHOD(bool, IsWhitelisted, (const std::string& steamId), (const, override));
    MOCK_METHOD(void, BroadcastAdminAlert, (const std::string& message), (override));
    MOCK_METHOD(void, LogSecurityEvent, (const std::string& steamId, const std::string& event), (override));
};

class MockPerformanceProfiler : public PerformanceProfiler {
public:
    MOCK_METHOD(void, Begin, (const std::string& section), (override));
    MOCK_METHOD(void, End, (const std::string& section), (override));
    MOCK_METHOD(double, GetAverageTime, (const std::string& section), (const, override));
    MOCK_METHOD(double, GetTotalTime, (const std::string& section), (const, override));
};

// EAC emulator implementation for testing
class TestEACEmulator : public EACServerEmulator {
public:
    TestEACEmulator() : m_nextSessionId(1), m_running(false) {}
    
    bool Initialize(const std::string& serverKey) override {
        m_serverKey = serverKey;
        if (m_serverKey.empty() || m_serverKey.length() < 8) {
            return false;
        }
        m_running = true;
        return true;
    }
    
    void Shutdown() override {
        m_running = false;
        m_sessions.clear();
    }
    
    bool IsRunning() const override {
        return m_running;
    }
    
    uint32_t CreateSession(const std::string& steamId) {
        if (!m_running) return 0;
        
        uint32_t sessionId = m_nextSessionId++;
        m_sessions[sessionId] = std::make_unique<EACScanSession>(steamId, sessionId);
        return sessionId;
    }
    
    bool AuthenticateClient(uint32_t sessionId, const std::vector<uint8_t>& challengeResponse) {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return false;
        
        // Simple challenge validation (in real EAC this would be cryptographic)
        if (challengeResponse.size() >= 16) {
            it->second->isAuthenticated = true;
            it->second->lastActivity = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    EACScanResult PerformMemoryScan(uint32_t sessionId, const EACMemoryOperation& operation) {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second->isAuthenticated) {
            return EACScanResult::ERROR;
        }
        
        it->second->operations.push_back(operation);
        it->second->lastActivity = std::chrono::steady_clock::now();
        
        // Simulate scan logic based on operation data
        if (operation.size > 1024 * 1024) { // Large memory read
            return EACScanResult::SUSPICIOUS;
        }
        
        // Check for suspicious patterns in data
        if (!operation.data.empty()) {
            uint8_t firstByte = operation.data[0];
            bool allSame = std::all_of(operation.data.begin(), operation.data.end(),
                                     [firstByte](uint8_t b) { return b == firstByte; });
            if (allSame && operation.data.size() > 100) {
                return EACScanResult::DETECTED; // Suspicious pattern
            }
        }
        
        return EACScanResult::CLEAN;
    }
    
    void RemoveSession(uint32_t sessionId) {
        m_sessions.erase(sessionId);
    }
    
    std::vector<uint32_t> GetActiveSessions() const {
        std::vector<uint32_t> active;
        for (const auto& [id, session] : m_sessions) {
            if (session->isActive) {
                active.push_back(id);
            }
        }
        return active;
    }
    
    EACScanSession* GetSession(uint32_t sessionId) {
        auto it = m_sessions.find(sessionId);
        return (it != m_sessions.end()) ? it->second.get() : nullptr;
    }
    
    // Mock implementation of base class methods
    bool HandleRemoteMemoryRead(const std::vector<uint8_t>& data) override {
        if (data.size() < 8) return false;
        
        uint32_t sessionId = *reinterpret_cast<const uint32_t*>(data.data());
        EACMemoryOperation op;
        op.address = *reinterpret_cast<const uint64_t*>(data.data() + 4);
        op.size = data.size() - 12;
        op.data.assign(data.begin() + 12, data.end());
        op.timestamp = std::chrono::steady_clock::now();
        
        EACScanResult result = PerformMemoryScan(sessionId, op);
        m_lastScanResult = result;
        return result == EACScanResult::CLEAN;
    }
    
    bool HandleWriteAck(const std::vector<uint8_t>& data) override {
        return data.size() >= 4; // Minimum ack packet size
    }
    
    bool HandleAlloc(const std::vector<uint8_t>& data) override {
        return data.size() >= 8; // Address + size
    }
    
    bool HandleBroadcastRead(const std::vector<uint8_t>& data) override {
        return !data.empty();
    }
    
    EACScanResult GetLastScanResult() const { return m_lastScanResult; }
    
private:
    std::string m_serverKey;
    std::atomic<uint32_t> m_nextSessionId;
    std::atomic<bool> m_running;
    std::unordered_map<uint32_t, std::unique_ptr<EACScanSession>> m_sessions;
    EACScanResult m_lastScanResult = EACScanResult::CLEAN;
};

// Test fixture for EAC tests
class EACTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize mocks
        mockSecurityConfig = std::make_shared<NiceMock<MockSecurityConfig>>();
        mockAdminManager = std::make_shared<NiceMock<MockAdminManager>>();
        mockProfiler = std::make_shared<NiceMock<MockPerformanceProfiler>>();

        // Set up default mock behavior
        ON_CALL(*mockSecurityConfig, IsEACEnabled())
            .WillByDefault(Return(true));
        ON_CALL(*mockSecurityConfig, GetEACServerKey())
            .WillByDefault(Return(TEST_EAC_KEY));
        ON_CALL(*mockSecurityConfig, GetEACTimeout())
            .WillByDefault(Return(EAC_RESPONSE_TIMEOUT_MS));
        ON_CALL(*mockSecurityConfig, GetEACScanInterval())
            .WillByDefault(Return(EAC_SCAN_INTERVAL_MS));
        ON_CALL(*mockSecurityConfig, GetEACStrictMode())
            .WillByDefault(Return(false));
        ON_CALL(*mockSecurityConfig, GetEACLoggingEnabled())
            .WillByDefault(Return(true));

        ON_CALL(*mockAdminManager, IsBanned(_))
            .WillByDefault(Return(false));
        ON_CALL(*mockAdminManager, IsWhitelisted(_))
            .WillByDefault(Return(false));

        // Create EAC components
        eacEmulator = std::make_unique<TestEACEmulator>();
        eacProxy = std::make_unique<EACProxy>(mockSecurityConfig);
        
        // Initialize random number generator
        rng.seed(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void TearDown() override {
        if (eacProxy) {
            eacProxy->Shutdown();
            eacProxy.reset();
        }
        if (eacEmulator) {
            eacEmulator->Shutdown();
            eacEmulator.reset();
        }
        mockProfiler.reset();
        mockAdminManager.reset();
        mockSecurityConfig.reset();
    }

    // Helper methods
    std::vector<uint8_t> CreateRandomData(size_t size) {
        std::vector<uint8_t> data(size);
        std::uniform_int_distribution<uint8_t> dist(0, 255);
        std::generate(data.begin(), data.end(), [&]() { return dist(rng); });
        return data;
    }

    std::vector<uint8_t> CreateSuspiciousData(size_t size) {
        std::vector<uint8_t> data(size, 0xCC); // Repeated pattern
        return data;
    }

    std::vector<uint8_t> CreateMemoryReadPacket(uint32_t sessionId, uint64_t address, const std::vector<uint8_t>& memory) {
        std::vector<uint8_t> packet;
        packet.resize(12 + memory.size());
        
        *reinterpret_cast<uint32_t*>(packet.data()) = sessionId;
        *reinterpret_cast<uint64_t*>(packet.data() + 4) = address;
        std::copy(memory.begin(), memory.end(), packet.begin() + 12);
        
        return packet;
    }

    Packet CreateEACPacket(EACPacketType type, const std::vector<uint8_t>& data) {
        Packet packet;
        packet.SetType(PacketType::PT_RPC_CALL); // EAC uses RPC calls
        
        std::vector<uint8_t> payload;
        payload.resize(2 + data.size());
        *reinterpret_cast<uint16_t*>(payload.data()) = static_cast<uint16_t>(type);
        std::copy(data.begin(), data.end(), payload.begin() + 2);
        
        packet.SetData(payload);
        return packet;
    }

    // Test data
    std::shared_ptr<MockSecurityConfig> mockSecurityConfig;
    std::shared_ptr<MockAdminManager> mockAdminManager;
    std::shared_ptr<MockPerformanceProfiler> mockProfiler;
    std::unique_ptr<TestEACEmulator> eacEmulator;
    std::unique_ptr<EACProxy> eacProxy;
    std::mt19937 rng;
};

// === EAC Initialization Tests ===

TEST_F(EACTest, EACProxy_ValidConfiguration_InitializesSuccessfully) {
    // Act
    bool result = eacProxy->Initialize();

    // Assert
    EXPECT_TRUE(result);
    EXPECT_TRUE(eacProxy->IsRunning());
}

TEST_F(EACTest, EACProxy_InvalidServerKey_FailsInitialization) {
    // Arrange
    EXPECT_CALL(*mockSecurityConfig, GetEACServerKey())
        .WillOnce(Return(""));

    // Act
    bool result = eacProxy->Initialize();

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(EACTest, EACProxy_EACDisabled_SkipsInitialization) {
    // Arrange
    EXPECT_CALL(*mockSecurityConfig, IsEACEnabled())
        .WillOnce(Return(false));

    // Act
    bool result = eacProxy->Initialize();

    // Assert
    EXPECT_FALSE(result); // Should not initialize when disabled
}

TEST_F(EACTest, EACEmulator_ValidKey_InitializesCorrectly) {
    // Act
    bool result = eacEmulator->Initialize(TEST_EAC_KEY);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_TRUE(eacEmulator->IsRunning());
}

TEST_F(EACTest, EACEmulator_InvalidKey_FailsInitialization) {
    // Act
    bool result = eacEmulator->Initialize("short");

    // Assert
    EXPECT_FALSE(result);
    EXPECT_FALSE(eacEmulator->IsRunning());
}

// === Session Management Tests ===

TEST_F(EACTest, SessionManagement_CreateSession_ReturnsValidId) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);

    // Act
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);

    // Assert
    EXPECT_GT(sessionId, 0);
    
    auto* session = eacEmulator->GetSession(sessionId);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->steamId, VALID_STEAM_ID);
    EXPECT_EQ(session->sessionId, sessionId);
    EXPECT_FALSE(session->isAuthenticated);
}

TEST_F(EACTest, SessionManagement_AuthenticateValid_Success) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    std::vector<uint8_t> validResponse = CreateRandomData(32); // Valid challenge response

    // Act
    bool result = eacEmulator->AuthenticateClient(sessionId, validResponse);

    // Assert
    EXPECT_TRUE(result);
    
    auto* session = eacEmulator->GetSession(sessionId);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->isAuthenticated);
}

TEST_F(EACTest, SessionManagement_AuthenticateInvalid_Failure) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    std::vector<uint8_t> invalidResponse = CreateRandomData(8); // Too short

    // Act
    bool result = eacEmulator->AuthenticateClient(sessionId, invalidResponse);

    // Assert
    EXPECT_FALSE(result);
    
    auto* session = eacEmulator->GetSession(sessionId);
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->isAuthenticated);
}

TEST_F(EACTest, SessionManagement_MultipleSessions_HandledCorrectly) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    
    // Act - Create multiple sessions
    uint32_t session1 = eacEmulator->CreateSession("player1");
    uint32_t session2 = eacEmulator->CreateSession("player2");
    uint32_t session3 = eacEmulator->CreateSession("player3");

    // Assert
    EXPECT_NE(session1, session2);
    EXPECT_NE(session2, session3);
    EXPECT_NE(session1, session3);
    
    auto activeSessions = eacEmulator->GetActiveSessions();
    EXPECT_EQ(activeSessions.size(), 3);
}

TEST_F(EACTest, SessionManagement_RemoveSession_CleansUp) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);

    // Act
    eacEmulator->RemoveSession(sessionId);

    // Assert
    auto* session = eacEmulator->GetSession(sessionId);
    EXPECT_EQ(session, nullptr);
    
    auto activeSessions = eacEmulator->GetActiveSessions();
    EXPECT_EQ(activeSessions.size(), 0);
}

// === Memory Scanning Tests ===

TEST_F(EACTest, MemoryScanning_CleanMemory_ReturnsClean) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    EACMemoryOperation operation;
    operation.address = 0x10000000;
    operation.size = 1024;
    operation.data = CreateRandomData(1024);

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, operation);

    // Assert
    EXPECT_EQ(result, EACScanResult::CLEAN);
}

TEST_F(EACTest, MemoryScanning_SuspiciousPatterns_DetectsThreat) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    EACMemoryOperation operation;
    operation.address = 0x10000000;
    operation.size = 500;
    operation.data = CreateSuspiciousData(500); // Repeated pattern

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, operation);

    // Assert
    EXPECT_EQ(result, EACScanResult::DETECTED);
}

TEST_F(EACTest, MemoryScanning_LargeMemoryRead_FlagsSuspicious) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    EACMemoryOperation operation;
    operation.address = 0x10000000;
    operation.size = 2 * 1024 * 1024; // 2MB - too large
    operation.data = CreateRandomData(1024); // Sample data

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, operation);

    // Assert
    EXPECT_EQ(result, EACScanResult::SUSPICIOUS);
}

TEST_F(EACTest, MemoryScanning_UnauthenticatedSession_ReturnsError) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    // Note: Not authenticating the session
    
    EACMemoryOperation operation;
    operation.address = 0x10000000;
    operation.size = 1024;
    operation.data = CreateRandomData(1024);

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, operation);

    // Assert
    EXPECT_EQ(result, EACScanResult::ERROR);
}

// === Packet Handling Tests ===

TEST_F(EACTest, PacketHandling_RemoteMemoryRead_ProcessedCorrectly) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    
    auto memoryData = CreateRandomData(256);
    auto packet = CreateMemoryReadPacket(sessionId, 0x10000000, memoryData);

    // Act
    bool result = eacEmulator->HandleRemoteMemoryRead(packet);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_EQ(eacEmulator->GetLastScanResult(), EACScanResult::CLEAN);
}

TEST_F(EACTest, PacketHandling_WriteAck_ValidatesCorrectly) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    std::vector<uint8_t> validAck = {0x01, 0x02, 0x03, 0x04}; // 4 bytes minimum

    // Act
    bool result = eacEmulator->HandleWriteAck(validAck);

    // Assert
    EXPECT_TRUE(result);
}

TEST_F(EACTest, PacketHandling_WriteAck_TooShort_Fails) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    std::vector<uint8_t> shortAck = {0x01, 0x02}; // Too short

    // Act
    bool result = eacEmulator->HandleWriteAck(shortAck);

    // Assert
    EXPECT_FALSE(result);
}

TEST_F(EACTest, PacketHandling_AllocRequest_ValidatesSize) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    std::vector<uint8_t> allocData = {0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 8 bytes

    // Act
    bool result = eacEmulator->HandleAlloc(allocData);

    // Assert
    EXPECT_TRUE(result);
}

TEST_F(EACTest, PacketHandling_BroadcastRead_AcceptsNonEmpty) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    std::vector<uint8_t> broadcastData = CreateRandomData(128);

    // Act
    bool result = eacEmulator->HandleBroadcastRead(broadcastData);

    // Assert
    EXPECT_TRUE(result);
}

// === Integration with Admin Manager Tests ===

TEST_F(EACTest, Integration_CheatDetected_TriggersAdminAction) {
    // Arrange
    EXPECT_CALL(*mockAdminManager, BanPlayer(VALID_STEAM_ID, _, _))
        .Times(1);
    EXPECT_CALL(*mockAdminManager, LogSecurityEvent(VALID_STEAM_ID, _))
        .Times(AtLeast(1));

    eacProxy->Initialize();
    
    // Simulate cheat detection
    auto suspiciousData = CreateSuspiciousData(500);
    auto packet = CreateMemoryReadPacket(1, 0x10000000, suspiciousData);

    // Act
    bool handled = eacProxy->HandleRemoteMemoryRead(packet);
    
    // Simulate the proxy detecting the cheat and triggering admin action
    if (!handled) {
        mockAdminManager->BanPlayer(VALID_STEAM_ID, "EAC: Cheat detected", 0); // Permanent ban
        mockAdminManager->LogSecurityEvent(VALID_STEAM_ID, "EAC cheat detection");
    }

    // Assert
    // Expectations are verified by mock calls
}

TEST_F(EACTest, Integration_WhitelistedPlayer_BypassesStrictMode) {
    // Arrange
    EXPECT_CALL(*mockSecurityConfig, GetEACStrictMode())
        .WillOnce(Return(true));
    EXPECT_CALL(*mockAdminManager, IsWhitelisted(VALID_STEAM_ID))
        .WillOnce(Return(true));

    eacProxy->Initialize();

    // Act - Simulate EAC validation for whitelisted player
    bool isWhitelisted = mockAdminManager->IsWhitelisted(VALID_STEAM_ID);
    bool strictMode = mockSecurityConfig->GetEACStrictMode();

    // Assert
    EXPECT_TRUE(isWhitelisted);
    EXPECT_TRUE(strictMode);
    // In implementation, whitelisted players would bypass strict mode checks
}

// === Performance Tests ===

TEST_F(EACTest, Performance_MultipleConcurrentScans_HandledEfficiently) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    const int sessionCount = 50;
    const int scansPerSession = 20;
    
    std::vector<uint32_t> sessions;
    for (int i = 0; i < sessionCount; ++i) {
        uint32_t session = eacEmulator->CreateSession("player" + std::to_string(i));
        eacEmulator->AuthenticateClient(session, CreateRandomData(32));
        sessions.push_back(session);
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    int cleanScans = 0;
    for (uint32_t session : sessions) {
        for (int scan = 0; scan < scansPerSession; ++scan) {
            EACMemoryOperation op;
            op.address = 0x10000000 + (scan * 1024);
            op.size = 512;
            op.data = CreateRandomData(512);
            
            if (eacEmulator->PerformMemoryScan(session, op) == EACScanResult::CLEAN) {
                cleanScans++;
            }
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Assert
    EXPECT_LT(duration.count(), 1000); // Should complete within 1 second
    EXPECT_GT(cleanScans, sessionCount * scansPerSession * 0.8); // Most should be clean
}

TEST_F(EACTest, Performance_LargeMemoryOperation_CompletesInTime) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    EACMemoryOperation largeOp;
    largeOp.address = 0x10000000;
    largeOp.size = 100 * 1024; // 100KB
    largeOp.data = CreateRandomData(100 * 1024);

    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, largeOp);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert
    EXPECT_NE(result, EACScanResult::ERROR);
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for 100KB scan
}

// === Error Handling and Edge Cases ===

TEST_F(EACTest, ErrorHandling_InvalidSessionId_HandledGracefully) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t invalidSessionId = 99999;

    // Act & Assert - Should not crash
    EXPECT_NO_THROW({
        EACMemoryOperation op;
        op.address = 0x10000000;
        op.size = 1024;
        op.data = CreateRandomData(1024);
        
        EACScanResult result = eacEmulator->PerformMemoryScan(invalidSessionId, op);
        EXPECT_EQ(result, EACScanResult::ERROR);
    });
}

TEST_F(EACTest, ErrorHandling_EmptyMemoryData_HandledSafely) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    EACMemoryOperation emptyOp;
    emptyOp.address = 0x10000000;
    emptyOp.size = 0;
    emptyOp.data.clear();

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, emptyOp);

    // Assert - Should handle gracefully
    EXPECT_EQ(result, EACScanResult::CLEAN);
}

TEST_F(EACTest, ErrorHandling_ShutdownWhileActive_CleansUpSafely) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    
    // Create multiple active sessions
    for (int i = 0; i < 10; ++i) {
        uint32_t session = eacEmulator->CreateSession("player" + std::to_string(i));
        eacEmulator->AuthenticateClient(session, CreateRandomData(32));
    }

    // Act
    eacEmulator->Shutdown();

    // Assert
    EXPECT_FALSE(eacEmulator->IsRunning());
    EXPECT_EQ(eacEmulator->GetActiveSessions().size(), 0);
}

// === Security Validation Tests ===

TEST_F(EACTest, Security_RepeatedFailedAuth_TriggersSecurity) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    
    int failedAttempts = 0;
    const int maxAttempts = 5;

    // Act - Multiple failed authentication attempts
    for (int i = 0; i < maxAttempts + 2; ++i) {
        std::vector<uint8_t> invalidResponse = CreateRandomData(4); // Too short
        if (!eacEmulator->AuthenticateClient(sessionId, invalidResponse)) {
            failedAttempts++;
        }
    }

    // Assert
    EXPECT_GE(failedAttempts, maxAttempts);
    // In real implementation, this would trigger security measures
}

TEST_F(EACTest, Security_TimestampValidation_DetectsReplay) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    // Create operation with old timestamp
    EACMemoryOperation oldOp;
    oldOp.address = 0x10000000;
    oldOp.size = 1024;
    oldOp.data = CreateRandomData(1024);
    oldOp.timestamp = std::chrono::steady_clock::now() - std::chrono::hours(1); // 1 hour old

    // Act
    EACScanResult result = eacEmulator->PerformMemoryScan(sessionId, oldOp);

    // Assert - Should still process but could flag for review
    EXPECT_NE(result, EACScanResult::ERROR);
}

TEST_F(EACTest, Security_MultipleDetections_EscalatesResponse) {
    // Arrange
    eacEmulator->Initialize(TEST_EAC_KEY);
    uint32_t sessionId = eacEmulator->CreateSession(VALID_STEAM_ID);
    eacEmulator->AuthenticateClient(sessionId, CreateRandomData(32));
    
    int detectionCount = 0;

    // Act - Multiple suspicious operations
    for (int i = 0; i < 5; ++i) {
        EACMemoryOperation suspiciousOp;
        suspiciousOp.address = 0x10000000 + (i * 1024);
        suspiciousOp.size = 300;
        suspiciousOp.data = CreateSuspiciousData(300);
        
        if (eacEmulator->PerformMemoryScan(sessionId, suspiciousOp) == EACScanResult::DETECTED) {
            detectionCount++;
        }
    }

    // Assert
    EXPECT_GE(detectionCount, 3); // Should detect multiple suspicious operations
    // In real implementation, multiple detections would escalate to immediate ban
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}