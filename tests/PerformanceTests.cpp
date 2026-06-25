// tests/PerformanceTests.cpp
// Comprehensive performance benchmarks for core subsystems
//
// Covers: packet analysis, handler dispatch, config access, memory pool, physics simulation.
// Uses GoogleTest for measurement and assertions.

#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include <random>

// Include headers for subsystems
#include "Utils/PacketAnalysis.h"
#include "Utils/HandlerLibraryManager.h"
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Utils/MemoryPool.h"
#include "Physics/PhysicsEngine.h"
#include "Math/Vector3.h"
#include "Game/GameMode.h"
#include "Utils/Logger.h"

using namespace std::chrono;

// Utility for timing
#define TIME_BLOCK(label, code) \
    { \
      auto _start = high_resolution_clock::now(); \
      code; \
      auto _end = high_resolution_clock::now(); \
      auto _ms = duration_cast<milliseconds>(_end - _start).count(); \
      Logger::Info("%s: %lld ms", label, (long long)_ms); \
      EXPECT_LT(_ms, maxDurations[#label]); \
    }

static const std::unordered_map<std::string, int> maxDurations = {
    {"PacketAnalysis", 500},          // ms for 1M packets
    {"HandlerDispatch", 200},         // ms for dispatching 100k calls
    {"ConfigAccess", 50},             // ms for 10k accesses
    {"MemoryPoolOps", 100},           // ms for 1M alloc/free
    {"PhysicsUpdate", 100},           // ms for 10k collision checks
    {"GameModeUpdate", 100}           // ms for 10k ticks
};

TEST(PerformanceTests, PacketAnalysis) {
    PacketAnalyzer analyzer;
    std::vector<uint8_t> buf(64, 0xAA);
    const int N = 1'000'000;
    TIME_BLOCK("PacketAnalysis",
      for (int i = 0; i < N; ++i) {
        PacketAnalysisResult res;
        analyzer.Analyze(buf.data(), buf.size(), res);
      }
    )
}

TEST(PerformanceTests, HandlerDispatch) {
    auto& mgr = HandlerLibraryManager::Instance();
    // Assume handlers loaded
    const int N = 100'000;
    PacketAnalysisResult dummy;
    TIME_BLOCK("HandlerDispatch",
      for (int i = 0; i < N; ++i) {
        auto h = mgr.GetHandler("Handle_HEARTBEAT");
        if (h) h(dummy);
      }
    )
}

TEST(PerformanceTests, ConfigAccess) {
    ConfigManager cfg;
    cfg.LoadConfiguration("server.ini");
    const int N = 10'000;
    TIME_BLOCK("ConfigAccess",
      for (int i = 0; i < N; ++i) {
        volatile auto v1 = cfg.GetString("Server", "Name", "");
        volatile auto v2 = cfg.GetInt("Server", "Port", 0);
        volatile auto v3 = cfg.GetBool("Server", "Debug", false);
      }
    )
}

TEST(PerformanceTests, MemoryPoolOps) {
    MemoryPool pool(64, 1024, 4096);
    const int N = 1'000'000;
    std::vector<void*> ptrs;
    ptrs.reserve(1000);
    TIME_BLOCK("MemoryPoolOps",
      for (int i = 0; i < N; ++i) {
        void* p = pool.Allocate();
        pool.Free(p);
      }
    )
}

TEST(PerformanceTests, PhysicsUpdate) {
    PhysicsEngine phys;
    phys.Initialize();
    std::vector<uint32_t> bodies;
    for (int i = 0; i < 1000; ++i) {
      bodies.push_back(phys.CreateRigidBody({float(i),0,0}, {1,1,1}, 1.0f));
    }
    const int N = 10000;
    TIME_BLOCK("PhysicsUpdate",
      for (int i = 0; i < N; ++i) {
        phys.Update(1.0f/60.0f);
      }
    )
    phys.Shutdown();
}

TEST(PerformanceTests, GameModeUpdate) {
    GameModeDefinition def{"Test", "desc", 32, 300.0f, 100, false,10.0f,{},{}};
    GameMode gm(nullptr, def);
    const int N = 10000;
    TIME_BLOCK("GameModeUpdate",
      for (int i = 0; i < N; ++i) {
        gm.Update();
      }
    )
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}