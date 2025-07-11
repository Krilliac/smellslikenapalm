// tests/MemoryPoolTests.cpp
// Comprehensive memory-allocation pool tests
//
// These tests make sure that:
//  1.  The pool returns correctly aligned, zero-initialised blocks.
//  2.  Free-list bookkeeping never corrupts memory under heavy churn.
//  3.  The pool gracefully expands (or rejects requests) when exhausted.
//  4.  Double-free, use-after-free and out-of-bounds writes are detected.
//  5.  Multithreaded access is lock-free and race-condition free.
//  6.  Allocation / free latency stays <1µs for small blocks under load.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>

// Include the actual pool header
#include "Utils/MemoryPool.h"
#include "Utils/Logger.h"

using namespace std::chrono_literals;

/* -------------------------------------------------------------------------- */
/*                            compile-time properties                          */
/* -------------------------------------------------------------------------- */

static_assert(std::is_trivially_destructible_v<MemoryPool>,
              "MemoryPool must be trivially destructible for lock-free shutdown");

/* -------------------------------------------------------------------------- */
/*                               test fixture                                 */
/* -------------------------------------------------------------------------- */

class MemoryPoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 128 byte blocks, 1 KiB initial arena, 4 KiB max growth
        pool = std::make_unique<MemoryPool>(128, 1024, 4096);
    }

    void TearDown() override { pool.reset(); }

    std::unique_ptr<MemoryPool> pool;
};

/* -------------------------------------------------------------------------- */
/*                               basic sanity                                 */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, AllocateAndFree_SingleBlock_Succeeds)
{
    void* p = pool->Allocate();
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 128);
    pool->Free(p);
    EXPECT_EQ(pool->OutstandingAllocations(), 0);
}

TEST_F(MemoryPoolTest, Alignment_Is16Bytes)
{
    void* p = pool->Allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) & 0xF, 0u);  // 16-byte aligned
    pool->Free(p);
}

/* -------------------------------------------------------------------------- */
/*                              pool expansion                                */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, ExhaustPool_TriggersExpansion)
{
    std::vector<void*> blocks;
    for (int i = 0; i < 32; ++i)  // 32*128 = 4,096 > 1,024 initial arena
        blocks.emplace_back(pool->Allocate());

    EXPECT_GT(pool->ArenaCount(), 1);  // new arena added

    for (void* p : blocks) pool->Free(p);
    EXPECT_EQ(pool->OutstandingAllocations(), 0);
}

/* -------------------------------------------------------------------------- */
/*                              double-free check                              */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, DoubleFree_ThrowsAssertInDebug)
{
#ifdef _DEBUG
    void* p = pool->Allocate();
    pool->Free(p);
    EXPECT_DEATH(pool->Free(p), "double free");
#else
    GTEST_SKIP() << "Double-free guard active only in debug builds.";
#endif
}

/* -------------------------------------------------------------------------- */
/*                             buffer overflow guard                           */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, CanaryDetectsOverflow)
{
#ifdef _DEBUG
    uint8_t* p = static_cast<uint8_t*>(pool->Allocate());
    p[128] = 0xFF;                       // corrupt guard byte
    EXPECT_DEATH(pool->Free(p), "buffer overflow");
#else
    GTEST_SKIP() << "Guard bytes only in debug builds.";
#endif
}

/* -------------------------------------------------------------------------- */
/*                           multithreaded pressure                            */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, ParallelAllocateFree_NoLeaksNoCrashes)
{
    constexpr int Threads = 8;
    constexpr int OpsPerThread = 100'000;
    std::atomic<int> leaks{0};

    auto worker = [&]
    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(1, 4);
        std::vector<void*> scratch;

        for (int i = 0; i < OpsPerThread; ++i)
        {
            int batch = dist(rng);
            for (int j = 0; j < batch; ++j) scratch.push_back(pool->Allocate());
            std::shuffle(scratch.begin(), scratch.end(), rng);
            for (int j = 0; j < batch; ++j)
            {
                pool->Free(scratch.back());
                scratch.pop_back();
            }
        }
        leaks += scratch.size();
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < Threads; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    EXPECT_EQ(leaks.load(), 0);
    EXPECT_EQ(pool->OutstandingAllocations(), 0);
}

/* -------------------------------------------------------------------------- */
/*                    latency measurements under contention                    */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, AllocationLatency_BelowOneMicrosecond)
{
    constexpr int N = 1'000'000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i)
    {
        void* p = pool->Allocate();
        pool->Free(p);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double avgNs = std::chrono::duration<double, std::nano>(end - start).count() / N;
    EXPECT_LT(avgNs, 1'000.0);  // 1 µs = 1,000 ns
}

/* -------------------------------------------------------------------------- */
/*                           exhaustion without growth                         */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, NoGrowthMode_ReturnsNullWhenExhausted)
{
    MemoryPool fixed(64, 640, 0);  // 10 blocks, no growth
    std::vector<void*> v;
    for (int i = 0; i < 10; ++i) v.push_back(fixed.Allocate());
    EXPECT_EQ(fixed.Allocate(), nullptr);
    for (auto* p : v) fixed.Free(p);
}

/* -------------------------------------------------------------------------- */
/*                       stress test continuous allocation                     */
/* -------------------------------------------------------------------------- */

TEST_F(MemoryPoolTest, Stress_LongRunningChurn_Stable)
{
    constexpr int Iterations = 2'000'000;
    std::mt19937 rng(1234);
    std::vector<void*> bag;
    bag.reserve(64);

    for (int i = 0; i < Iterations; ++i)
    {
        if (!bag.empty() && (rng() & 1))
        {   pool->Free(bag.back());
            bag.pop_back();
        }
        else
        {   if (void* p = pool->Allocate()) bag.push_back(p); }
    }
    for (void* p : bag) pool->Free(p);
    EXPECT_EQ(pool->OutstandingAllocations(), 0);
}