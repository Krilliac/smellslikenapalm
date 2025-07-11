// tests/NetworkTests.cpp
// Comprehensive network subsystem unit tests
//
// Tests cover packet serialization/deserialization, socket management,
// rate limiting, error handling, and boundary conditions.
// Uses GoogleTest and GoogleMock.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

#include "Network/NetworkManager.h"
#include "Network/PacketSerializer.h"
#include "Network/Packet.h"
#include "Network/NetworkUtils.h"
#include "Config/NetworkConfig.h"
#include "Utils/Logger.h"

using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::Invoke;

// Mock NetworkConfig for rate limiting and timeouts
class MockNetworkConfig : public NetworkConfig {
public:
    MOCK_METHOD(double, GetMaxBandwidthMbps, (), (const, override));
    MOCK_METHOD(int, GetPacketTimeout, (), (const, override));
    MOCK_METHOD(int, GetHeartbeatInterval, (), (const, override));
    MOCK_METHOD(int, GetMaxPacketsPerTick, (), (const, override));
    MOCK_METHOD(size_t, GetMTU, (), (const, override));
    MOCK_METHOD(bool, IsCompressionEnabled, (), (const, override));
};

// Fixture for PacketSerializer
class PacketSerializerTest : public ::testing::Test {
protected:
    PacketSerializer serializer;
};

TEST_F(PacketSerializerTest, SerializeDeserialize_HeaderRoundTrip) {
    uint16_t tag = ProtocolUtils::TypeToTag(PacketType::PT_CHAT_MESSAGE);
    uint32_t length = 128;
    auto header = serializer.SerializeHeader(tag, length);
    ASSERT_TRUE(serializer.ValidateHeader(header.data(), header.size()));
    EXPECT_EQ(serializer.GetTag(header.data(), header.size()), PacketType::PT_CHAT_MESSAGE);
    EXPECT_EQ(serializer.GetLength(header.data(), header.size()), length);
}

TEST_F(PacketSerializerTest, InvalidHeader_FailsValidation) {
    auto header = serializer.SerializeHeader(0, 0);
    header[0] = 0x00;  // corrupt magic
    EXPECT_FALSE(serializer.ValidateHeader(header.data(), header.size()));
}

TEST_F(PacketSerializerTest, BufferSizeLimits) {
    size_t mtu = serializer.GetMaxPacketSize();
    EXPECT_TRUE(serializer.ValidateBufferSize(mtu));
    EXPECT_FALSE(serializer.ValidateBufferSize(mtu + 1));
}

// Fixture for NetworkManager
class MockNetworkConfigForManager : public NetworkConfig {
public:
    MOCK_METHOD(int, GetPort, (), (const, override));
    MOCK_METHOD(size_t, GetMTU, (), (const, override));
    MOCK_METHOD(int, GetPacketTimeout, (), (const, override));
    MOCK_METHOD(int, GetHeartbeatInterval, (), (const, override));
    MOCK_METHOD(int, GetMaxPacketsPerTick, (), (const, override));
};

class NetworkManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config = std::make_shared<StrictMock<MockNetworkConfigForManager>>();
        EXPECT_CALL(*config, GetPort()).WillRepeatedly(Return(9000));
        EXPECT_CALL(*config, GetMTU()).WillRepeatedly(Return(512));
        EXPECT_CALL(*config, GetPacketTimeout()).WillRepeatedly(Return(2000));
        EXPECT_CALL(*config, GetHeartbeatInterval()).WillRepeatedly(Return(1000));
        EXPECT_CALL(*config, GetMaxPacketsPerTick()).WillRepeatedly(Return(100));
        manager = std::make_unique<NetworkManager>(config);
    }

    std::shared_ptr<MockNetworkConfigForManager> config;
    std::unique_ptr<NetworkManager> manager;
};

TEST_F(NetworkManagerTest, InitializeAndShutdown_Succeeds) {
    EXPECT_TRUE(manager->Initialize());
    EXPECT_NO_THROW(manager->Shutdown());
}

TEST_F(NetworkManagerTest, SendReceiveLoopEcho) {
    // Initialize two managers bound to different ports for echo test
    auto config1 = config;
    auto config2 = std::make_shared<StrictMock<MockNetworkConfigForManager>>();
    EXPECT_CALL(*config2, GetPort()).WillRepeatedly(Return(9001));
    EXPECT_CALL(*config2, GetMTU()).WillRepeatedly(Return(512));
    EXPECT_CALL(*config2, GetPacketTimeout()).WillRepeatedly(Return(2000));
    EXPECT_CALL(*config2, GetHeartbeatInterval()).WillRepeatedly(Return(1000));
    EXPECT_CALL(*config2, GetMaxPacketsPerTick()).WillRepeatedly(Return(100));
    NetworkManager mgr1(config1), mgr2(config2);
    ASSERT_TRUE(mgr1.Initialize());
    ASSERT_TRUE(mgr2.Initialize());

    std::vector<uint8_t> payload = { 'h','e','l','l','o' };
    mgr1.SendPacket(9001, payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto rec = mgr2.ReceivePackets();
    ASSERT_FALSE(rec.empty());
    EXPECT_EQ(rec[0].packet.GetData(), payload);

    mgr1.Shutdown();
    mgr2.Shutdown();
}

TEST_F(NetworkManagerTest, RateLimiting_DropsExcessPackets) {
    // Simulate sending more than max per tick and ensure drops occur
    int maxPerTick = config->GetMaxPacketsPerTick();
    std::atomic<int> sent{0}, dropped{0};
    for (int i = 0; i < maxPerTick + 50; ++i) {
        if (!manager->SendPacket(9000, std::vector<uint8_t>(10))) {
            dropped++;
        } else {
            sent++;
        }
    }
    EXPECT_EQ(sent.load(), maxPerTick);
    EXPECT_EQ(dropped.load(), 50);
}

// Fixture for Packet end-to-end
class PacketIntegrationTest : public ::testing::Test {
protected:
    PacketIntegrationTest() : serializer() {}

    Packet CreatePacket(PacketType type, const std::vector<uint8_t>& data) {
        Packet p;
        p.SetType(type);
        p.SetData(data);
        return p;
    }

    PacketSerializer serializer;
};

TEST_F(PacketIntegrationTest, PacketSerializeDeserialize_FullRoundTrip) {
    auto orig = CreatePacket(PacketType::PT_PLAYER_MOVE, std::vector<uint8_t>(100, 0x42));
    auto buf = serializer.Serialize(orig);
    auto parsed = serializer.Deserialize(buf);
    EXPECT_EQ(parsed.GetType(), PacketType::PT_PLAYER_MOVE);
    EXPECT_EQ(parsed.GetData(), orig.GetData());
}

// Utility boundary tests
TEST(UtilsTests, Checksum_ZeroAndNonZero) {
    std::vector<uint8_t> data1(100, 0);
    uint32_t cs1 = NetworkUtils::ComputeChecksum(data1.data(), data1.size());
    EXPECT_NE(cs1, 0u);

    std::vector<uint8_t> data2(100, 0xFF);
    uint32_t cs2 = NetworkUtils::ComputeChecksum(data2.data(), data2.size());
    EXPECT_NE(cs2, cs1);
}

// Error condition: invalid port
TEST_F(NetworkManagerTest, Initialize_InvalidPort_Fails) {
    EXPECT_CALL(*config, GetPort()).WillOnce(Return(-1));
    NetworkManager badMgr(config);
    EXPECT_FALSE(badMgr.Initialize());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}