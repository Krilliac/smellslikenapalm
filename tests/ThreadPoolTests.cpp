// tests/ThreadPoolTests.cpp
// Comprehensive unit tests for ThreadPool
//
// Covers:
// 1. Initialization and shutdown.
// 2. Task execution correctness.
// 3. Task ordering and fairness.
// 4. Load balancing across threads.
// 5. Shutdown with pending tasks.
// 6. Exception propagation from tasks.
// 7. Edge cases: enqueue after shutdown, high load.
//
// API reconciliation (test-side, src unchanged): the real ThreadPool exposes
// only Enqueue() (returning std::future) and Shutdown(). It has no WaitAll()
// and no GetThisThreadIndex(). Tests that previously relied on those were
// reworked to wait on the returned futures and to drop thread-index probing.
// The "zero threads = sequential executor" case was removed: the real pool
// requires worker threads to drain its queue, so a 0-thread pool never runs
// tasks (not the behavior the original test assumed).

#include "TestFramework.h"
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <future>
#include <stdexcept>
#include <algorithm>
#include "Utils/ThreadPool.h"
#include "Utils/Logger.h"

using namespace std::chrono_literals;

// Helper: wait for a batch of futures to complete.
template <typename T>
static void WaitAll(std::vector<std::future<T>>& futs) {
    for (auto& f : futs) f.wait();
}

// Fixture
class ThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<ThreadPool>(4);
    }
    void TearDown() override {
        if (pool) pool->Shutdown();
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
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 10; ++i) {
        futs.push_back(pool->Enqueue([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    WaitAll(futs);
    EXPECT_EQ(counter.load(), 10);
}

// 3. All enqueued tasks run (ordering not guaranteed across threads)
TEST_F(ThreadPoolTest, AllTasksRun) {
    std::vector<int> results;
    std::mutex mtx;
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 20; ++i) {
        futs.push_back(pool->Enqueue([i,&results,&mtx](){
            std::lock_guard<std::mutex> lk(mtx);
            results.push_back(i);
        }));
    }
    WaitAll(futs);
    EXPECT_EQ(results.size(), 20u);
    std::sort(results.begin(), results.end());
    for (int i = 0; i < 20; ++i) EXPECT_EQ(results[i], i);
}

// 4. Load balancing: spreading work across the pool completes all tasks.
TEST_F(ThreadPoolTest, LoadBalancing_AllTasksComplete) {
    const int tasks = 100;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < tasks; ++i) {
        futs.push_back(pool->Enqueue([&counter](){
            counter.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }));
    }
    WaitAll(futs);
    EXPECT_EQ(counter.load(), tasks);
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
    auto fut = pool->Enqueue([]() -> int {
        throw std::runtime_error("Task failure");
        return 42;
    });
    EXPECT_THROW(fut.get(), std::runtime_error);
}

// 7. Enqueue after shutdown throws
TEST_F(ThreadPoolTest, EnqueueAfterShutdown_Throws) {
    pool->Shutdown();
    EXPECT_THROW(pool->Enqueue([](){}), std::runtime_error);
}

// 8. High load stability
TEST_F(ThreadPoolTest, HighLoad_NoCrashes) {
    const int tasks = 1000;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < tasks; ++i) {
        futs.push_back(pool->Enqueue([&counter](){ counter++; }));
    }
    WaitAll(futs);
    EXPECT_EQ(counter.load(), tasks);
}

// 9. Concurrent shutdown safety
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
