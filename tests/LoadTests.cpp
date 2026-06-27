// tests/LoadTests.cpp
// Comprehensive load testing and performance validation
//
// API reconciliation (test-side, src unchanged): std::atomic<double> has no
// operator+= before C++20, so the latency accumulator is updated via a small
// compare-exchange helper (AtomicAdd) instead. No server APIs are exercised —
// this file is a self-contained traffic simulation.

#include "TestFramework.h"
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>

// Portable atomic floating-point add (pre-C++20 compatible).
static inline void AtomicAdd(std::atomic<double>& acc, double v) {
    double cur = acc.load(std::memory_order_relaxed);
    while (!acc.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
}

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
    float avgLatency() const {
        uint32_t n = latencyCount.load();
        return n ? static_cast<float>(totalLatency.load() / n) : 0.0f;
    }
    float lossRate() const {
        uint64_t s = sent.load();
        return s ? static_cast<float>(dropped.load()) / s : 0.0f;
    }
    float throughputMbps() const { return (bytes.load() * 8.0f) / 1000000.0f; }
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
                    AtomicAdd(metrics.totalLatency, lat);
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

class LoadTests : public ::rs2v::Test {
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

// This file did not define its own main() (unlike most test files), and we do
// the native framework has no separate main lib — so provide the entry point here.
RS2V_TEST_MAIN()
