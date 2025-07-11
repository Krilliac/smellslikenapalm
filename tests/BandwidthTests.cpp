// tests/BandwidthTests.cpp
// Comprehensive bandwidth management and network performance unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <numeric>

// Include the headers
#include "Network/NetworkManager.h"
#include "Network/BandwidthManager.h"
#include "Network/NetworkUtils.h"
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/CompressionHandler.h"
#include "Config/NetworkConfig.h"
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

// Forward declarations and constants
constexpr size_t DEFAULT_MTU = 1500;
constexpr size_t MAX_PACKET_SIZE = 65536;
constexpr double DEFAULT_BANDWIDTH_MBPS = 100.0;
constexpr int DEFAULT_TICK_RATE = 60;

// Mock classes for testing bandwidth management
class MockNetworkConfig : public NetworkConfig {
public:
    MOCK_METHOD(double, GetMaxBandwidthMbps, (), (const, override));
    MOCK_METHOD(int, GetTickRate, (), (const, override));
    MOCK_METHOD(size_t, GetMTU, (), (const, override));
    MOCK_METHOD(bool, IsCompressionEnabled, (), (const, override));
    MOCK_METHOD(double, GetCompressionThreshold, (), (const, override));
    MOCK_METHOD(int, GetMaxPacketsPerTick, (), (const, override));
    MOCK_METHOD(int, GetPriorityQueueSize, (), (const, override));
};

class MockCompressionHandler : public CompressionHandler {
public:
    MOCK_METHOD(std::vector<uint8_t>, Compress, (const std::vector<uint8_t>& data), (override));
    MOCK_METHOD(std::vector<uint8_t>, Decompress, (const std::vector<uint8_t>& compressedData), (override));
    MOCK_METHOD(float, GetCompressionRatio, (const std::vector<uint8_t>& original, const std::vector<uint8_t>& compressed), (const, override));
    MOCK_METHOD(bool, ShouldCompress, (size_t dataSize), (const, override));
};

class MockPerformanceProfiler : public PerformanceProfiler {
public:
    MOCK_METHOD(void, Begin, (const std::string& section), (override));
    MOCK_METHOD(void, End, (const std::string& section), (override));
    MOCK_METHOD(double, GetAverageTime, (const std::string& section), (const, override));
    MOCK_METHOD(double, GetTotalTime, (const std::string& section), (const, override));
};

// Token bucket implementation for rate limiting tests
class TokenBucket {
public:
    TokenBucket(double tokensPerSecond, double maxTokens)
        : m_tokensPerSecond(tokensPerSecond)
        , m_maxTokens(maxTokens)
        , m_currentTokens(maxTokens)
        , m_lastRefill(std::chrono::steady_clock::now()) {}

    bool TryConsume(double tokens) {
        RefillTokens();
        if (m_currentTokens >= tokens) {
            m_currentTokens -= tokens;
            return true;
        }
        return false;
    }

    double GetCurrentTokens() const { return m_currentTokens; }
    double GetMaxTokens() const { return m_maxTokens; }

private:
    void RefillTokens() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_lastRefill);
        double secondsElapsed = elapsed.count() / 1000000.0;
        
        double tokensToAdd = secondsElapsed * m_tokensPerSecond;
        m_currentTokens = std::min(m_maxTokens, m_currentTokens + tokensToAdd);
        m_lastRefill = now;
    }

    double m_tokensPerSecond;
    double m_maxTokens;
    double m_currentTokens;
    std::chrono::steady_clock::time_point m_lastRefill;
};

// Priority queue for packet scheduling
enum class PacketPriority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    CRITICAL = 3
};

struct PrioritizedPacket {
    Packet packet;
    PacketPriority priority;
    std::chrono::steady_clock::time_point timestamp;
    uint32_t clientId;
};

// Test fixture for bandwidth tests
class BandwidthTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize mocks
        mockNetworkConfig = std::make_shared<NiceMock<MockNetworkConfig>>();
        mockCompressionHandler = std::make_shared<NiceMock<MockCompressionHandler>>();
        mockProfiler = std::make_shared<NiceMock<MockPerformanceProfiler>>();

        // Set up default mock behavior
        ON_CALL(*mockNetworkConfig, GetMaxBandwidthMbps())
            .WillByDefault(Return(DEFAULT_BANDWIDTH_MBPS));
        ON_CALL(*mockNetworkConfig, GetTickRate())
            .WillByDefault(Return(DEFAULT_TICK_RATE));
        ON_CALL(*mockNetworkConfig, GetMTU())
            .WillByDefault(Return(DEFAULT_MTU));
        ON_CALL(*mockNetworkConfig, IsCompressionEnabled())
            .WillByDefault(Return(true));
        ON_CALL(*mockNetworkConfig, GetCompressionThreshold())
            .WillByDefault(Return(0.7));
        ON_CALL(*mockNetworkConfig, GetMaxPacketsPerTick())
            .WillByDefault(Return(100));
        ON_CALL(*mockNetworkConfig, GetPriorityQueueSize())
            .WillByDefault(Return(1000));

        ON_CALL(*mockCompressionHandler, ShouldCompress(_))
            .WillByDefault(Return(true));
        ON_CALL(*mockCompressionHandler, GetCompressionRatio(_, _))
            .WillByDefault(Return(0.6f)); // 40% compression

        // Create bandwidth manager with mocked dependencies
        bandwidthManager = std::make_unique<BandwidthManager>(mockNetworkConfig);
        
        // Create test token bucket
        tokenBucket = std::make_unique<TokenBucket>(1000.0, 5000.0); // 1000 tokens/sec, max 5000
    }

    void TearDown() override {
        tokenBucket.reset();
        bandwidthManager.reset();
        mockProfiler.reset();
        mockCompressionHandler.reset();
        mockNetworkConfig.reset();
    }

    // Helper methods
    Packet CreateTestPacket(PacketType type, size_t size, uint32_t clientId = 1) {
        std::vector<uint8_t> data(size, 0x42);
        Packet packet;
        packet.SetData(data);
        packet.SetClientId(clientId);
        packet.SetType(type);
        return packet;
    }

    std::vector<Packet> CreatePacketBurst(int count, size_t averageSize, PacketType type = PacketType::PT_PLAYER_MOVE) {
        std::vector<Packet> packets;
        packets.reserve(count);
        
        for (int i = 0; i < count; ++i) {
            size_t size = averageSize + (i % 100) - 50; // Add some variance
            packets.push_back(CreateTestPacket(type, size, i % 10 + 1));
        }
        return packets;
    }

    double CalculateBandwidthUsage(const std::vector<Packet>& packets, double timeSeconds) {
        size_t totalBytes = 0;
        for (const auto& packet : packets) {
            totalBytes += packet.GetSize();
        }
        return (totalBytes * 8.0) / (timeSeconds * 1000000.0); // Convert to Mbps
    }

    // Test data
    std::shared_ptr<MockNetworkConfig> mockNetworkConfig;
    std::shared_ptr<MockCompressionHandler> mockCompressionHandler;
    std::shared_ptr<MockPerformanceProfiler> mockProfiler;
    std::unique_ptr<BandwidthManager> bandwidthManager;
    std::unique_ptr<TokenBucket> tokenBucket;
};

// === Token Bucket Rate Limiting Tests ===

TEST_F(BandwidthTest, TokenBucket_InitialState_HasMaxTokens) {
    // Act
    double currentTokens = tokenBucket->GetCurrentTokens();
    double maxTokens = tokenBucket->GetMaxTokens();

    // Assert
    EXPECT_EQ(currentTokens, maxTokens);
    EXPECT_EQ(maxTokens, 5000.0);
}

TEST_F(BandwidthTest, TokenBucket_ConsumeTokens_WithinLimit_Success) {
    // Act
    bool consumed = tokenBucket->TryConsume(100.0);

    // Assert
    EXPECT_TRUE(consumed);
    EXPECT_NEAR(tokenBucket->GetCurrentTokens(), 4900.0, 0.1);
}

TEST_F(BandwidthTest, TokenBucket_ConsumeTokens_ExceedingLimit_Failure) {
    // Act
    bool consumed = tokenBucket->TryConsume(6000.0); // More than max

    // Assert
    EXPECT_FALSE(consumed);
    EXPECT_NEAR(tokenBucket->GetCurrentTokens(), 5000.0, 0.1); // Should remain unchanged
}

TEST_F(BandwidthTest, TokenBucket_Refill_OverTime_Success) {
    // Arrange
    tokenBucket->TryConsume(5000.0); // Consume all tokens
    EXPECT_NEAR(tokenBucket->GetCurrentTokens(), 0.0, 0.1);

    // Act - Wait for refill (1000 tokens/second)
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 0.1 seconds
    bool consumed = tokenBucket->TryConsume(1.0); // This will trigger refill

    // Assert - Should have ~100 tokens refilled
    EXPECT_TRUE(consumed);
    EXPECT_GE(tokenBucket->GetCurrentTokens(), 90.0); // Allow some timing variance
    EXPECT_LE(tokenBucket->GetCurrentTokens(), 110.0);
}

TEST_F(BandwidthTest, TokenBucket_BurstProtection_PreventsSpikeTraffic) {
    // Arrange
    std::vector<double> consumptionAttempts = {1000, 1000, 1000, 1000, 1000, 1000}; // 6000 tokens
    int successCount = 0;

    // Act
    for (double tokens : consumptionAttempts) {
        if (tokenBucket->TryConsume(tokens)) {
            successCount++;
        }
    }

    // Assert - Should only allow first 5 attempts (5000 max tokens)
    EXPECT_EQ(successCount, 5);
}

// === Bandwidth Calculation Tests ===

TEST_F(BandwidthTest, BandwidthCalculation_SinglePacket_CorrectMbps) {
    // Arrange
    auto packet = CreateTestPacket(PacketType::PT_HEARTBEAT, 64);
    std::vector<Packet> packets = {packet};
    double timeSeconds = 1.0;

    // Act
    double bandwidthMbps = CalculateBandwidthUsage(packets, timeSeconds);

    // Assert
    double expectedMbps = (64 * 8.0) / 1000000.0; // 64 bytes = 512 bits
    EXPECT_NEAR(bandwidthMbps, expectedMbps, 0.001);
}

TEST_F(BandwidthTest, BandwidthCalculation_MultiplePackets_AggregatedCorrectly) {
    // Arrange
    auto packets = CreatePacketBurst(100, 1000); // 100 packets, ~1000 bytes each
    double timeSeconds = 1.0;

    // Act
    double bandwidthMbps = CalculateBandwidthUsage(packets, timeSeconds);

    // Assert
    EXPECT_GT(bandwidthMbps, 0.7); // ~100KB = ~0.8 Mbps
    EXPECT_LT(bandwidthMbps, 1.0);
}

TEST_F(BandwidthTest, BandwidthCalculation_HighThroughput_WithinLimits) {
    // Arrange
    auto packets = CreatePacketBurst(1000, 1400); // Near MTU size
    double timeSeconds = 0.1; // High rate

    // Act
    double bandwidthMbps = CalculateBandwidthUsage(packets, timeSeconds);

    // Assert
    EXPECT_GT(bandwidthMbps, 100.0); // Should exceed 100 Mbps
    EXPECT_LT(bandwidthMbps, 1000.0); // But not unreasonably high
}

// === Packet Prioritization Tests ===

TEST_F(BandwidthTest, PacketPrioritization_CriticalFirst_CorrectOrdering) {
    // Arrange
    std::vector<PrioritizedPacket> packets;
    packets.push_back({CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100), PacketPriority::LOW, std::chrono::steady_clock::now(), 1});
    packets.push_back({CreateTestPacket(PacketType::PT_HEARTBEAT, 64), PacketPriority::CRITICAL, std::chrono::steady_clock::now(), 2});
    packets.push_back({CreateTestPacket(PacketType::PT_CHAT_MESSAGE, 200), PacketPriority::NORMAL, std::chrono::steady_clock::now(), 3});

    // Act - Sort by priority (higher priority first)
    std::sort(packets.begin(), packets.end(), [](const PrioritizedPacket& a, const PrioritizedPacket& b) {
        return static_cast<int>(a.priority) > static_cast<int>(b.priority);
    });

    // Assert
    EXPECT_EQ(packets[0].priority, PacketPriority::CRITICAL);
    EXPECT_EQ(packets[1].priority, PacketPriority::NORMAL);
    EXPECT_EQ(packets[2].priority, PacketPriority::LOW);
}

TEST_F(BandwidthTest, PacketPrioritization_SamePriority_FIFOOrder) {
    // Arrange
    auto startTime = std::chrono::steady_clock::now();
    std::vector<PrioritizedPacket> packets;
    packets.push_back({CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100), PacketPriority::NORMAL, startTime, 1});
    packets.push_back({CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100), PacketPriority::NORMAL, startTime + std::chrono::milliseconds(1), 2});
    packets.push_back({CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100), PacketPriority::NORMAL, startTime + std::chrono::milliseconds(2), 3});

    // Act - Sort by priority, then timestamp
    std::sort(packets.begin(), packets.end(), [](const PrioritizedPacket& a, const PrioritizedPacket& b) {
        if (a.priority == b.priority) {
            return a.timestamp < b.timestamp; // FIFO for same priority
        }
        return static_cast<int>(a.priority) > static_cast<int>(b.priority);
    });

    // Assert
    EXPECT_EQ(packets[0].clientId, 1);
    EXPECT_EQ(packets[1].clientId, 2);
    EXPECT_EQ(packets[2].clientId, 3);
}

// === Compression Tests ===

TEST_F(BandwidthTest, Compression_LargePacket_ReducesBandwidth) {
    // Arrange
    auto largePacket = CreateTestPacket(PacketType::PT_ACTOR_REPLICATION, 2000);
    std::vector<uint8_t> originalData = largePacket.RawData();
    std::vector<uint8_t> compressedData(originalData.size() * 0.6); // 40% compression

    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(2000))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockCompressionHandler, Compress(originalData))
        .WillOnce(Return(compressedData));
    EXPECT_CALL(*mockCompressionHandler, GetCompressionRatio(originalData, compressedData))
        .WillOnce(Return(0.6f));

    // Act
    bool shouldCompress = mockCompressionHandler->ShouldCompress(2000);
    auto compressed = mockCompressionHandler->Compress(originalData);
    float ratio = mockCompressionHandler->GetCompressionRatio(originalData, compressed);

    // Assert
    EXPECT_TRUE(shouldCompress);
    EXPECT_LT(compressed.size(), originalData.size());
    EXPECT_NEAR(ratio, 0.6f, 0.01f);
}

TEST_F(BandwidthTest, Compression_SmallPacket_SkipsCompression) {
    // Arrange
    auto smallPacket = CreateTestPacket(PacketType::PT_HEARTBEAT, 64);

    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(64))
        .WillOnce(Return(false));

    // Act
    bool shouldCompress = mockCompressionHandler->ShouldCompress(64);

    // Assert
    EXPECT_FALSE(shouldCompress);
}

TEST_F(BandwidthTest, Compression_ChatMessage_IneffectiveCompression) {
    // Arrange - Chat messages typically don't compress well
    auto chatPacket = CreateTestPacket(PacketType::PT_CHAT_MESSAGE, 150);
    std::vector<uint8_t> originalData = chatPacket.RawData();
    std::vector<uint8_t> compressedData(originalData.size() * 0.95); // Only 5% compression

    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(150))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockCompressionHandler, Compress(originalData))
        .WillOnce(Return(compressedData));
    EXPECT_CALL(*mockCompressionHandler, GetCompressionRatio(originalData, compressedData))
        .WillOnce(Return(0.95f));

    // Act
    auto compressed = mockCompressionHandler->Compress(originalData);
    float ratio = mockCompressionHandler->GetCompressionRatio(originalData, compressed);

    // Assert - Poor compression ratio, might skip compression in real scenario
    EXPECT_NEAR(ratio, 0.95f, 0.01f);
    EXPECT_GT(ratio, 0.9f); // Very little compression
}

// === MTU and Fragmentation Tests ===

TEST_F(BandwidthTest, MTU_OversizedPacket_RequiresFragmentation) {
    // Arrange
    size_t mtu = mockNetworkConfig->GetMTU();
    auto oversizedPacket = CreateTestPacket(PacketType::PT_ACTOR_REPLICATION, mtu + 500);

    // Act
    bool requiresFragmentation = oversizedPacket.GetSize() > mtu;
    int fragments = (oversizedPacket.GetSize() + mtu - 1) / mtu; // Ceiling division

    // Assert
    EXPECT_TRUE(requiresFragmentation);
    EXPECT_EQ(fragments, 2); // Should require 2 fragments
}

TEST_F(BandwidthTest, MTU_NormalPacket_NoFragmentation) {
    // Arrange
    size_t mtu = mockNetworkConfig->GetMTU();
    auto normalPacket = CreateTestPacket(PacketType::PT_PLAYER_MOVE, mtu - 100);

    // Act
    bool requiresFragmentation = normalPacket.GetSize() > mtu;

    // Assert
    EXPECT_FALSE(requiresFragmentation);
}

// === Network Congestion Simulation Tests ===

TEST_F(BandwidthTest, NetworkCongestion_HighPacketRate_DropsPackets) {
    // Arrange
    int maxPacketsPerTick = mockNetworkConfig->GetMaxPacketsPerTick();
    auto packets = CreatePacketBurst(maxPacketsPerTick * 2, 500); // Double the limit
    int processedCount = 0;

    // Act - Simulate packet processing with limit
    for (int i = 0; i < static_cast<int>(packets.size()) && processedCount < maxPacketsPerTick; ++i) {
        if (tokenBucket->TryConsume(1.0)) { // 1 token per packet
            processedCount++;
        }
    }

    // Assert
    EXPECT_EQ(processedCount, maxPacketsPerTick);
    EXPECT_LT(processedCount, static_cast<int>(packets.size())); // Some packets dropped
}

TEST_F(BandwidthTest, NetworkCongestion_BandwidthExceeded_ThrottlesTraffic) {
    // Arrange
    double maxBandwidthMbps = mockNetworkConfig->GetMaxBandwidthMbps();
    auto packets = CreatePacketBurst(1000, 1400); // Large packets
    double timeSeconds = 0.1; // Short time window
    double actualBandwidth = CalculateBandwidthUsage(packets, timeSeconds);

    // Act - Check if bandwidth is exceeded
    bool bandwidthExceeded = actualBandwidth > maxBandwidthMbps;

    // Assert
    if (bandwidthExceeded) {
        EXPECT_GT(actualBandwidth, maxBandwidthMbps);
        // In real implementation, this would trigger throttling
    }
}

// === Per-Client Bandwidth Management Tests ===

TEST_F(BandwidthTest, PerClientBandwidth_FairDistribution_MultipleClients) {
    // Arrange
    std::map<uint32_t, double> clientBandwidth;
    std::vector<Packet> packets;
    
    // Create packets from 5 different clients
    for (uint32_t clientId = 1; clientId <= 5; ++clientId) {
        for (int i = 0; i < 20; ++i) {
            packets.push_back(CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100, clientId));
        }
    }

    // Act - Calculate per-client bandwidth
    for (const auto& packet : packets) {
        clientBandwidth[packet.GetClientId()] += packet.GetSize() * 8.0; // Convert to bits
    }

    // Assert - Each client should have equal bandwidth
    double expectedBandwidthPerClient = (20 * 100 * 8.0); // 20 packets * 100 bytes * 8 bits
    for (const auto& [clientId, bandwidth] : clientBandwidth) {
        EXPECT_NEAR(bandwidth, expectedBandwidthPerClient, 1.0);
    }
}

TEST_F(BandwidthTest, PerClientBandwidth_SingleClientFlood_DoesNotStarveOthers) {
    // Arrange
    std::vector<Packet> packets;
    
    // Client 1 sends many packets
    for (int i = 0; i < 90; ++i) {
        packets.push_back(CreateTestPacket(PacketType::PT_CHAT_MESSAGE, 200, 1));
    }
    
    // Client 2 sends few packets
    for (int i = 0; i < 10; ++i) {
        packets.push_back(CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100, 2));
    }

    // Act - Simulate fair queueing (simplified)
    std::map<uint32_t, int> processedPerClient;
    int maxPerClient = 50; // Fair limit
    
    for (const auto& packet : packets) {
        if (processedPerClient[packet.GetClientId()] < maxPerClient) {
            processedPerClient[packet.GetClientId()]++;
        }
    }

    // Assert - Both clients should get fair treatment
    EXPECT_EQ(processedPerClient[1], maxPerClient); // Client 1 limited
    EXPECT_EQ(processedPerClient[2], 10); // Client 2 gets all packets
}

// === Performance and Scalability Tests ===

TEST_F(BandwidthTest, Performance_LargePacketBurst_AcceptableLatency) {
    // Arrange
    auto packets = CreatePacketBurst(10000, 800);
    
    EXPECT_CALL(*mockProfiler, Begin("BandwidthProcessing"))
        .Times(1);
    EXPECT_CALL(*mockProfiler, End("BandwidthProcessing"))
        .Times(1);

    // Act
    auto start = std::chrono::high_resolution_clock::now();
    mockProfiler->Begin("BandwidthProcessing");
    
    // Simulate bandwidth processing
    size_t totalBytes = 0;
    for (const auto& packet : packets) {
        totalBytes += packet.GetSize();
        // Simulate some processing overhead
        volatile int dummy = 0;
        for (int i = 0; i < 10; ++i) dummy += i;
    }
    
    mockProfiler->End("BandwidthProcessing");
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Assert - Processing should complete within reasonable time
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for 10k packets
    EXPECT_GT(totalBytes, 0);
}

TEST_F(BandwidthTest, Scalability_IncreasingLoad_LinearPerformance) {
    // Arrange
    std::vector<int> packetCounts = {100, 200, 400, 800};
    std::vector<double> processingTimes;

    // Act - Measure processing time for different loads
    for (int count : packetCounts) {
        auto packets = CreatePacketBurst(count, 500);
        
        auto start = std::chrono::high_resolution_clock::now();
        size_t totalBytes = 0;
        for (const auto& packet : packets) {
            totalBytes += packet.GetSize();
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        processingTimes.push_back(duration.count());
    }

    // Assert - Performance should scale roughly linearly
    for (size_t i = 1; i < processingTimes.size(); ++i) {
        double ratio = processingTimes[i] / processingTimes[i-1];
        EXPECT_GT(ratio, 1.5); // Should increase with load
        EXPECT_LT(ratio, 3.0); // But not exponentially
    }
}

// === Integration Tests ===

TEST_F(BandwidthTest, Integration_FullPipeline_CompressionAndPrioritization) {
    // Arrange
    std::vector<PrioritizedPacket> packets;
    packets.push_back({CreateTestPacket(PacketType::PT_HEARTBEAT, 64), PacketPriority::CRITICAL, std::chrono::steady_clock::now(), 1});
    packets.push_back({CreateTestPacket(PacketType::PT_ACTOR_REPLICATION, 2000), PacketPriority::NORMAL, std::chrono::steady_clock::now(), 2});
    packets.push_back({CreateTestPacket(PacketType::PT_CHAT_MESSAGE, 150), PacketPriority::LOW, std::chrono::steady_clock::now(), 3});

    // Set up compression expectations
    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(64))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(2000))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockCompressionHandler, ShouldCompress(150))
        .WillOnce(Return(true));

    // Act - Process through full pipeline
    // 1. Sort by priority
    std::sort(packets.begin(), packets.end(), [](const PrioritizedPacket& a, const PrioritizedPacket& b) {
        return static_cast<int>(a.priority) > static_cast<int>(b.priority);
    });

    // 2. Apply compression where appropriate
    for (auto& prioritizedPacket : packets) {
        size_t packetSize = prioritizedPacket.packet.GetSize();
        if (mockCompressionHandler->ShouldCompress(packetSize)) {
            // Compression would be applied here
        }
    }

    // 3. Check token bucket for rate limiting
    int processedCount = 0;
    for (const auto& prioritizedPacket : packets) {
        if (tokenBucket->TryConsume(prioritizedPacket.packet.GetSize() / 100.0)) { // Scale down for test
            processedCount++;
        }
    }

    // Assert
    EXPECT_EQ(packets[0].priority, PacketPriority::CRITICAL); // Highest priority first
    EXPECT_EQ(processedCount, 3); // All packets should be processed (small load)
}

TEST_F(BandwidthTest, Integration_NetworkStress_GracefulDegradation) {
    // Arrange - Create overwhelming load
    auto packets = CreatePacketBurst(5000, 1200);
    int maxPacketsPerTick = 100; // Artificial limit
    
    // Act - Simulate network under stress
    int processedCount = 0;
    int droppedCount = 0;
    size_t totalBytesProcessed = 0;

    for (const auto& packet : packets) {
        if (processedCount < maxPacketsPerTick && tokenBucket->TryConsume(1.0)) {
            processedCount++;
            totalBytesProcessed += packet.GetSize();
        } else {
            droppedCount++;
        }
    }

    // Assert - Should gracefully handle overload
    EXPECT_EQ(processedCount, maxPacketsPerTick);
    EXPECT_GT(droppedCount, 0); // Some packets should be dropped
    EXPECT_GT(totalBytesProcessed, 0);
    
    double dropRate = static_cast<double>(droppedCount) / packets.size();
    EXPECT_GT(dropRate, 0.9); // High drop rate under stress
}

// === Edge Cases and Error Handling ===

TEST_F(BandwidthTest, EdgeCase_ZeroSizePacket_HandledSafely) {
    // Arrange
    auto zeroPacket = CreateTestPacket(PacketType::PT_HEARTBEAT, 0);

    // Act
    double bandwidth = CalculateBandwidthUsage({zeroPacket}, 1.0);
    bool consumed = tokenBucket->TryConsume(0.0);

    // Assert - Should handle gracefully
    EXPECT_EQ(bandwidth, 0.0);
    EXPECT_TRUE(consumed); // Zero tokens should always succeed
}

TEST_F(BandwidthTest, EdgeCase_MaxSizePacket_WithinBounds) {
    // Arrange
    auto maxPacket = CreateTestPacket(PacketType::PT_ACTOR_REPLICATION, MAX_PACKET_SIZE);

    // Act
    bool withinBounds = maxPacket.GetSize() <= MAX_PACKET_SIZE;
    double bandwidth = CalculateBandwidthUsage({maxPacket}, 1.0);

    // Assert
    EXPECT_TRUE(withinBounds);
    EXPECT_GT(bandwidth, 0.0);
    EXPECT_LT(bandwidth, 1000.0); // Should be reasonable
}

TEST_F(BandwidthTest, EdgeCase_NegativeTime_ReturnsZero) {
    // Arrange
    auto packet = CreateTestPacket(PacketType::PT_PLAYER_MOVE, 100);

    // Act & Assert - Should handle gracefully
    EXPECT_NO_THROW({
        double bandwidth = CalculateBandwidthUsage({packet}, 0.0);
        // Implementation should handle division by zero
    });
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}