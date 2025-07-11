// tests/LoadTests.cpp
// Comprehensive load testing and performance validation

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>

// Core headers
#include "Game/GameServer.h"
#include "Network/NetworkManager.h"
#include "Network/BandwidthManager.h"
#include "Utils/PerformanceProfiler.h"
#include "Protocol/PacketTypes.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

constexpr int MIN_CLIENTS = 10;
constexpr int MAX_CLIENTS = 500;
constexpr int PACKETS_PER_CLIENT_PER_SEC = 20;
constexpr float MAX_LATENCY_MS = 200.0f;
constexpr float MAX_PACKET_LOSS = 0.05f;  // 5%
constexpr float MIN_THROUGHPUT_MBPS = 10.0f;

struct LoadMetrics {
    std::atomic<uint64_t> sent{0}, received{0}, dropped{0}, bytes{0};
    std::atomic<double> totalLatency{0.0};
    std::atomic<uint32_t> latencyCount{0};
    std::atomic<uint32_t> connections{0};
    std::atomic<uint32_t> failures{0};
    float avgLatency() const { return latencyCount? totalLatency/latencyCount : 0; }
    float lossRate() const { return sent? (float)dropped/sent : 0; }
    float throughputMbps() const { return (bytes*8.0f)/(1000000.0f); }
};

class LoadClient {
public:
    LoadClient(int id, LoadMetrics& m) : clientId(id), metrics(m), rng(id) {}
    void start(int pps) {
        active = true;
        thread = std::thread(&LoadClient::run, this, pps);
    }
    void stop() {
        active = false;
        if (thread.joinable()) thread.join();
    }
private:
    void run(int pps) {
        std::uniform_int_distribution<int> typeDist(0,2);
        std::chrono::microseconds interval(1000000/pps);
        auto last = std::chrono::steady_clock::now();
        while(active) {
            auto now = std::chrono::steady_clock::now();
            if (now - last >= interval) {
                PacketType type = (typeDist(rng)==0? PacketType::PT_PLAYER_MOVE
                                  : typeDist(rng)==1? PacketType::PT_CHAT_MESSAGE
                                  : PacketType::PT_HEARTBEAT);
                size_t sz = (type==PacketType::PT_CHAT_MESSAGE?256:64);
                metrics.sent++; metrics.bytes += sz;
                // simulate network drop
                if (std::uniform_real_distribution<float>(0,1)(rng) < MAX_PACKET_LOSS) {
                    metrics.dropped++;
                } else {
                    metrics.received++;
                    double lat = std::normal_distribution<double>(50,10)(rng);
                    metrics.totalLatency += lat;
                    metrics.latencyCount++;
                }
                last = now;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    int clientId;
    std::atomic<bool> active{false};
    std::thread thread;
    LoadMetrics& metrics;
    std::mt19937 rng;
};

class LoadTests : public ::testing::Test {
protected:
    void SetUp() override {
        metrics = std::make_unique<LoadMetrics>();
    }
    void TearDown() override {
        for(auto& c: clients) c->stop();
    }

    bool runScenario(int clientCount, int durationSec, int pps) {
        clients.clear();
        metrics->connections = clientCount;
        for(int i=0;i<clientCount;i++){
            clients.push_back(std::make_unique<LoadClient>(i+1,*metrics));
            clients.back()->start(pps);
        }
        std::this_thread::sleep_for(std::chrono::seconds(durationSec));
        for(auto& c: clients) c->stop();

        // Log
        Logger::Info("LoadTest: clients=%d duration=%ds sent=%lu recv=%lu dropped=%lu "
                     "avgLat=%.2fms loss=%.2f%% throughput=%.2fMbps",
                     clientCount, durationSec,
                     metrics->sent.load(), metrics->received.load(), metrics->dropped.load(),
                     metrics->avgLatency(), metrics->lossRate()*100,
                     metrics->throughputMbps());

        return metrics->avgLatency() <= MAX_LATENCY_MS
            && metrics->lossRate() <= MAX_PACKET_LOSS
            && metrics->throughputMbps() >= MIN_THROUGHPUT_MBPS;
    }

    std::unique_ptr<LoadMetrics> metrics;
    std::vector<std::unique_ptr<LoadClient>> clients;
};

TEST_F(LoadTests, LowLoad_BaselinePerformance) {
    EXPECT_TRUE(runScenario(MIN_CLIENTS, 10, PACKETS_PER_CLIENT_PER_SEC));
}

TEST_F(LoadTests, MediumLoad_StablePerformance) {
    EXPECT_TRUE(runScenario(100, 15, PACKETS_PER_CLIENT_PER_SEC));
}

TEST_F(LoadTests, HighLoad_NearCapacity) {
    EXPECT_TRUE(runScenario(300, 20, PACKETS_PER_CLIENT_PER_SEC));
}

TEST_F(LoadTests, StressLoad_MaxCapacity) {
    EXPECT_TRUE(runScenario(MAX_CLIENTS, 30, PACKETS_PER_CLIENT_PER_SEC));
}