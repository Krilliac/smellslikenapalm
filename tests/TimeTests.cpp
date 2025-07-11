// tests/TimeTests.cpp
// Comprehensive unit tests for time-related utilities and scheduling
//
// Covers:
// 1. High-resolution timer accuracy.
// 2. Scheduling tasks with Timer class (single-shot, repeating).
// 3. Deadline and timeout utilities.
// 4. Time formatting and parsing (ISO8601).
// 5. Edge cases: zero and negative intervals.
// 6. Thread safety of timers under concurrent use.
// 7. Performance: scheduling many timers.

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include "Utils/Timer.h"
#include "Utils/TimeUtils.h"
#include "Utils/Logger.h"

using namespace std::chrono;

// Fixture for high-resolution timer
class HighResTimerTest : public ::testing::Test {
protected:
    void SetUp() override {
        start = high_resolution_clock::now();
    }
    high_resolution_clock::time_point start;
};

TEST_F(HighResTimerTest, TimerResolution_IsMonotonic) {
    auto t1 = high_resolution_clock::now();
    auto t2 = high_resolution_clock::now();
    EXPECT_LE(t1.time_since_epoch().count(), t2.time_since_epoch().count());
}

TEST_F(HighResTimerTest, TimerSleep_YieldsApproxDuration) {
    auto before = high_resolution_clock::now();
    std::this_thread::sleep_for(50ms);
    auto after = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(after - before).count();
    EXPECT_GE(elapsed, 50);
    EXPECT_LE(elapsed, 80);  // allow some overhead
}

// Fixture for Timer class
class TimerTest : public ::testing::Test {
protected:
    Timer timer;
};

TEST_F(TimerTest, SingleShot_ExecutesOnce) {
    std::atomic<int> count{0};
    timer.ScheduleOnce(100ms, [&](){ count++; });
    std::this_thread::sleep_for(200ms);
    timer.CancelAll();
    EXPECT_EQ(count.load(), 1);
}

TEST_F(TimerTest, RepeatingTimer_ExecutesMultipleTimes) {
    std::atomic<int> count{0};
    timer.ScheduleRepeating(50ms, 100ms, [&](){ count++; });
    std::this_thread::sleep_for(350ms);
    timer.CancelAll();
    EXPECT_GE(count.load(), 2);
    EXPECT_LE(count.load(), 5);
}

TEST_F(TimerTest, CancelBeforeFire_NoExecution) {
    std::atomic<bool> fired{false};
    auto id = timer.ScheduleOnce(100ms, [&](){ fired = true; });
    timer.Cancel(id);
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(fired.load());
}

TEST_F(TimerTest, ZeroInterval_RepeatingImmediate) {
    std::atomic<int> count{0};
    timer.ScheduleRepeating(0ms, 0ms, [&](){
        if (++count > 5) timer.CancelAll();
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_GT(count.load(), 0);
}

// Deadline and timeout utilities
TEST(TimeUtilsTest, Deadline_PastDeadline) {
    auto past = high_resolution_clock::now() - 10ms;
    EXPECT_FALSE(TimeUtils::WaitUntil(past, [](){ return true; }, 50ms));
}

TEST(TimeUtilsTest, WaitUntil_SucceedsBeforeDeadline) {
    auto deadline = high_resolution_clock::now() + 100ms;
    bool result = TimeUtils::WaitUntil(deadline, [](){
        static int cnt=0;
        return (++cnt > 2);
    }, 150ms);
    EXPECT_TRUE(result);
}

// Time formatting/parsing
TEST(TimeFormatTest, ISO8601_RoundTrip) {
    auto now = system_clock::now();
    std::string s = TimeUtils::ToISO8601(now);
    auto parsed = TimeUtils::FromISO8601(s);
    auto diff = duration_cast<seconds>(now - parsed).count();
    EXPECT_LE(std::abs(diff), 1);
}

TEST(TimeFormatTest, ParseInvalidString_Throws) {
    EXPECT_THROW(TimeUtils::FromISO8601("not-a-date"), std::runtime_error);
}

// Thread safety under concurrent scheduling
TEST_F(TimerTest, ConcurrentTimers_NoRace) {
    std::atomic<int> count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&](){
            timer.ScheduleOnce(20ms, [&](){ count++; });
        });
    }
    for (auto& t : threads) t.join();
    std::this_thread::sleep_for(100ms);
    timer.CancelAll();
    EXPECT_EQ(count.load(), 10);
}

// Performance: many timers
TEST_F(TimerTest, Performance_ManyTimers_ScheduleExecute) {
    constexpr int N = 10000;
    std::atomic<int> count{0};
    auto start = high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        timer.ScheduleOnce(1ms, [&](){ count++; });
    }
    // allow execution
    std::this_thread::sleep_for(200ms);
    timer.CancelAll();
    auto end = high_resolution_clock::now();
    double ms = duration_cast<milliseconds>(end - start).count();
    Logger::Info("Scheduled %d timers in %.2f ms", N, ms);
    EXPECT_EQ(count.load(), N);
    EXPECT_LT(ms, 200.0);  // scheduling overhead <200ms
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}