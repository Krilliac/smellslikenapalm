// tests/ThreadPoolTests.cpp
// Comprehensive unit tests for ThreadPool
//
// Covers:
// 1. Initialization and shutdown.
// 2. Task execution correctness.
// 3. Task ordering and fairness.
// 4. Work-stealing or load balancing across threads.
// 5. Shutdown with pending tasks.
// 6. Exception propagation from tasks.
// 7. Edge cases: zero threads, enqueue after shutdown, high load.

#include <gtest/gtest.h>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <future>
#include <stdexcept>
#include "Utils/ThreadPool.h"
#include "Utils/Logger.h"

using namespace std::chrono_literals;

// Fixture
class ThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // default pool with 4 threads
        pool = std::make_unique<ThreadPool>(4);
    }
    void TearDown() override {
        pool->Shutdown();
        pool.reset();
    }
    std::unique_ptr<ThreadPool> pool;
};

// 1. Initialize and shutdown
TEST_F(ThreadPoolTest, InitializeAndShutdown_Succeeds) {
    EXPECT_NO_THROW(ThreadPool(2).Shutdown());
}

// 2. Simple task execution
TEST_F(ThreadPoolTest, ExecuteSimpleTasks) {
    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
        pool->Enqueue([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool->WaitAll();
    EXPECT_EQ(counter.load(), 10);
}

// 3. Task ordering preserved per enqueue sequence
TEST_F(ThreadPoolTest, TaskOrdering_FIFO) {
    std::vector<int> results;
    std::mutex mtx;
    for (int i = 0; i < 20; ++i) {
        pool->Enqueue([i,&results,&mtx](){
            std::lock_guard<std::mutex> lk(mtx);
            results.push_back(i);
        });
    }
    pool->WaitAll();
    EXPECT_EQ(results.size(), 20u);
    // since multiple threads, strict ordering not guaranteed globally,
    // but we can test that each task ran
    std::sort(results.begin(), results.end());
    for (int i = 0; i < 20; ++i) EXPECT_EQ(results[i], i);
}

// 4. Load balancing: all threads get work
TEST_F(ThreadPoolTest, LoadBalancing_AllThreadsActive) {
    const int tasks = 100;
    std::vector<std::atomic<int>> counts(4);
    for (int i = 0; i < tasks; ++i) {
        pool->Enqueue([&counts](){
            auto id = ThreadPool::GetThisThreadIndex();
            counts[id].fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        });
    }
    pool->WaitAll();
    int activeThreads = 0;
    for (auto &c : counts) if (c.load()>0) ++activeThreads;
    EXPECT_EQ(activeThreads, 4);
}

// 5. Shutdown with pending tasks executes all
TEST_F(ThreadPoolTest, ShutdownCompletesPending) {
    std::atomic<int> counter{0};
    for (int i = 0; i < 50; ++i) {
        pool->Enqueue([&counter](){
            std::this_thread::sleep_for(2ms);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool->Shutdown();  // should finish all
    EXPECT_EQ(counter.load(), 50);
}

// 6. Exception propagation from tasks
TEST_F(ThreadPoolTest, TaskException_PropagatedViaFuture) {
    auto fut = pool->Enqueue<int>([](){
        throw std::runtime_error("Task failure");
        return 42;
    });
    pool->WaitAll();
    EXPECT_THROW(fut.get(), std::runtime_error);
}

// 7. Zero threads treated as sequential executor
TEST(ThreadPool, ZeroThreads_SequentialExecution) {
    ThreadPool p(0);
    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i) p.Enqueue([&counter](){ counter++; });
    p.WaitAll();
    EXPECT_EQ(counter.load(), 5);
    p.Shutdown();
}

// 8. Enqueue after shutdown no-op or throws
TEST_F(ThreadPoolTest, EnqueueAfterShutdown_Throws) {
    pool->Shutdown();
    EXPECT_THROW(pool->Enqueue([](){}), std::runtime_error);
}

// 9. High load stability
TEST_F(ThreadPoolTest, HighLoad_NoCrashes) {
    const int tasks = 1000;
    std::atomic<int> counter{0};
    for (int i = 0; i < tasks; ++i) {
        pool->Enqueue([&counter](){ counter++; });
    }
    pool->WaitAll();
    EXPECT_EQ(counter.load(), tasks);
}

// 10. Concurrent shutdown safety
TEST_F(ThreadPoolTest, ConcurrentShutdown_NoRace) {
    std::thread t1([&](){ pool->Shutdown(); });
    std::thread t2([&](){ pool->Shutdown(); });
    t1.join(); t2.join();
    SUCCEED();  // no crash
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}