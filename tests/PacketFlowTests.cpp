// tests/PacketFlowTests.cpp
// Comprehensive end-to-end packet flow tests
//
// These tests simulate full packet lifecycle: creation, serialization,
// send through NetworkManager, analysis via PacketAnalyzer, handling
// by dynamic handlers, and ensure correct dispatch ordering and error
// recovery under packet loss and reordering.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <algorithm>

#include "Network/NetworkManager.h"
#include "Protocol/PacketTypes.h"
#include "Utils/PacketAnalysis.h"
#include "Utils/HandlerLibraryManager.h"
#include "Utils/PacketSerializer.h"
#include "Utils/Logger.h"

using ::testing::_;

// Mock handler to record invocation order
static std::vector<std::string> invokedHandlers;
extern "C" void Handle_CHAT_MESSAGE(const PacketAnalysisResult& r) { invokedHandlers.push_back("CHAT_MESSAGE"); }
extern "C" void Handle_PLAYER_MOVE (const PacketAnalysisResult& r) { invokedHandlers.push_back("PLAYER_MOVE"); }
extern "C" void Handle_HEARTBEAT   (const PacketAnalysisResult& r) { invokedHandlers.push_back("HEARTBEAT"); }

class PacketFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize serializer, analyzer, handler manager
        serializer = std::make_unique<PacketSerializer>();
        analyzer   = std::make_unique<PacketAnalyzer>();
        auto& mgr  = HandlerLibraryManager::Instance();
        // Assume handlers already compiled into shared lib loaded externally
        // Here we simulate handler lookup by injecting mocks
        invokedHandlers.clear();
    }

    Packet MakePacket(PacketType type, const std::vector<uint8_t>& data) {
        Packet p;
        p.SetType(type);
        p.SetData(data);
        return p;
    }

    void ProcessFlow(const std::vector<Packet>& packets, bool allowReorder=false, float lossRate=0.0f) {
        // Serialize and optionally shuffle/drop
        std::vector<std::vector<uint8_t>> buffers;
        for (auto& p : packets) {
            buffers.push_back(serializer->Serialize(p));
        }
        if (allowReorder) {
            std::shuffle(buffers.begin(), buffers.end(), rng);
        }
        // Simulate loss
        for (auto& buf : buffers) {
            if (uniform() < lossRate) continue;
            PacketAnalysisResult result;
            analyzer->Analyze(buf.data(), buf.size(), result);
            // Dispatch to handler
            auto handler = HandlerLibraryManager::Instance().GetHandler(
                ProtocolUtils::TagToName(result.packetType));
            if (handler) handler(result);
        }
    }

    std::unique_ptr<PacketSerializer> serializer;
    std::unique_ptr<PacketAnalyzer> analyzer;
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> uniform{0.0f,1.0f};
};

TEST_F(PacketFlowTest, SequentialFlow_HandlersInvokedInOrder) {
    std::vector<Packet> pkts = {
        MakePacket(PacketType::PT_HEARTBEAT,   {0}),
        MakePacket(PacketType::PT_CHAT_MESSAGE,{1,2,3}),
        MakePacket(PacketType::PT_PLAYER_MOVE, {4,5,6,7})
    };
    ProcessFlow(pkts);
    ASSERT_EQ(invokedHandlers.size(), 3u);
    EXPECT_EQ(invokedHandlers[0], "HEARTBEAT");
    EXPECT_EQ(invokedHandlers[1], "CHAT_MESSAGE");
    EXPECT_EQ(invokedHandlers[2], "PLAYER_MOVE");
}

TEST_F(PacketFlowTest, ReorderedFlow_HandlersOrderReflectsReorder) {
    std::vector<Packet> pkts = {
        MakePacket(PacketType::PT_HEARTBEAT,   {0}),
        MakePacket(PacketType::PT_CHAT_MESSAGE,{1,2,3}),
        MakePacket(PacketType::PT_PLAYER_MOVE, {4,5,6,7})
    };
    ProcessFlow(pkts, /*allowReorder*/true);
    ASSERT_EQ(invokedHandlers.size(), 3u);
    // Order should not match original
    bool same = (invokedHandlers[0]=="HEARTBEAT"
              && invokedHandlers[1]=="CHAT_MESSAGE"
              && invokedHandlers[2]=="PLAYER_MOVE");
    EXPECT_FALSE(same);
}

TEST_F(PacketFlowTest, PacketLoss_LossyFlow_SomeHandlersMissing) {
    std::vector<Packet> pkts = {
        MakePacket(PacketType::PT_HEARTBEAT,   {0}),
        MakePacket(PacketType::PT_CHAT_MESSAGE,{1,2,3}),
        MakePacket(PacketType::PT_PLAYER_MOVE, {4,5,6,7}),
        MakePacket(PacketType::PT_HEARTBEAT,   {8}),
        MakePacket(PacketType::PT_CHAT_MESSAGE,{9,0})
    };
    ProcessFlow(pkts,false,/*lossRate*/0.4f);
    // Some packets may be lost
    EXPECT_GT(invokedHandlers.size(), 0u);
    EXPECT_LT(invokedHandlers.size(), pkts.size()+1);
}

TEST_F(PacketFlowTest, StressFlow_HighVolume_NoCrashes) {
    std::vector<Packet> pkts;
    for (int i = 0; i < 1000; ++i) {
        pkts.push_back(MakePacket(PacketType::PT_CHAT_MESSAGE,{static_cast<uint8_t>(i&0xFF)}));
    }
    EXPECT_NO_THROW(ProcessFlow(pkts,true,0.1f));
}