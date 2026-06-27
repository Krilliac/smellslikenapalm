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

RS2V_TEST_MAIN()
