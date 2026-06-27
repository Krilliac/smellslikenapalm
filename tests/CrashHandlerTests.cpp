// tests/CrashHandlerTests.cpp
//
// Tests for the NON-FATAL exception handling added to the crash handler:
// rs2v::Guard runs a callable, swallows + reports any exception (so it does not
// reach std::terminate), and returns whether it completed cleanly;
// rs2v::NonFatalExceptionCount tracks how many were recovered. (The fatal path —
// std::set_terminate / signal handlers — intentionally aborts the process and so
// cannot be unit-tested in-process.)

#include "TestFramework.h"

#include "Utils/CrashHandler.h"

#include <stdexcept>
#include <string>
#include <thread>

TEST(CrashHandler, GuardReturnsTrueWhenCallableSucceeds) {
    bool ran = false;
    bool ok = rs2v::Guard("test-ok", [&] { ran = true; });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(ran);
}

TEST(CrashHandler, GuardSwallowsStdExceptionAndCountsIt) {
    unsigned long long before = rs2v::NonFatalExceptionCount();
    bool ok = rs2v::Guard("test-throw-std", [] {
        throw std::runtime_error("boom");
    });
    EXPECT_FALSE(ok);
    EXPECT_EQ(rs2v::NonFatalExceptionCount(), before + 1);
}

TEST(CrashHandler, GuardSwallowsNonStdExceptionAndCountsIt) {
    unsigned long long before = rs2v::NonFatalExceptionCount();
    bool ok = rs2v::Guard("test-throw-int", [] { throw 42; });
    EXPECT_FALSE(ok);
    EXPECT_EQ(rs2v::NonFatalExceptionCount(), before + 1);
}

TEST(CrashHandler, GuardReturnValuePropagatesAcrossManyCalls) {
    unsigned long long before = rs2v::NonFatalExceptionCount();
    int successes = 0;
    for (int i = 0; i < 5; ++i) {
        bool ok = rs2v::Guard("loop", [i] {
            if (i % 2 == 0) throw std::logic_error("even");
        });
        if (ok) ++successes;
    }
    // i = 1, 3 succeed; i = 0, 2, 4 throw.
    EXPECT_EQ(successes, 2);
    EXPECT_EQ(rs2v::NonFatalExceptionCount(), before + 3);
}

// Regression test for the wiring added across the server's thread entry points
// (NetworkThread / EACServerEmulator run loops, Timer callbacks, the auto-regen
// thread, the protocol-decode worker). Each of those guards a callable that runs
// on a worker thread; an uncaught exception there would call std::terminate and
// kill the whole process. This proves Guard stays terminate-safe when the throw
// originates off the main thread — if it did not, the std::terminate would abort
// the test binary and this test would never report PASS.
TEST(CrashHandler, GuardSwallowsExceptionThrownOnWorkerThread) {
    unsigned long long before = rs2v::NonFatalExceptionCount();
    bool ok = true;
    std::thread worker([&] {
        ok = rs2v::Guard("worker-thread-throw", [] {
            throw std::runtime_error("thrown on a worker thread");
        });
    });
    worker.join();
    EXPECT_FALSE(ok);
    EXPECT_EQ(rs2v::NonFatalExceptionCount(), before + 1);
}

RS2V_TEST_MAIN()
