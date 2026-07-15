// tests/FrameTimingTests.cpp
// Unit tests for measured game-loop delta normalization.

#include "TestFramework.h"

#include "Time/FrameTiming.h"

#include <limits>

TEST(FrameTiming, UsesMeasuredElapsedTimeUnderLoad) {
    // A 32 Hz loaded loop must advance by its real 31.25 ms, not the nominal
    // 16.67 ms. This is the regression behind a 30 s phase taking ~56 s.
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(0.03125f, 1.0f / 60.0f),
                    0.03125f);
}

TEST(FrameTiming, BoundsLongPauseToOneSafeStep) {
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(5.0f, 1.0f / 60.0f),
                    FrameTiming::kMaxTickDeltaSeconds);
}

TEST(FrameTiming, FallsBackToNominalForDirectDeterministicTicks) {
    const float nominal = 1.0f / 30.0f;
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(-1.0f, nominal), nominal);
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(0.0f, nominal), nominal);
    EXPECT_FLOAT_EQ(
        FrameTiming::ResolveTickDeltaSeconds(std::numeric_limits<float>::quiet_NaN(), nominal),
        nominal);
}

TEST(FrameTiming, PreservesDeliberatelySlowConfiguredTick) {
    // The safety cap limits unexpected pauses, not an explicit 2 Hz server.
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(-1.0f, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(FrameTiming::ResolveTickDeltaSeconds(0.75f, 0.5f), 0.5f);
}

RS2V_TEST_MAIN()
