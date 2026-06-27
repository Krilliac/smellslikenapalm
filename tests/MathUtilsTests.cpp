// tests/MathUtilsTests.cpp
//
// Native unit tests for src/Utils/MathUtils.h — the small numeric helpers used
// by gameplay/physics math. Covers the deterministic, pure functions (the
// RNG helpers are intentionally not asserted on exact values).

#include "TestFramework.h"

#include "Utils/MathUtils.h"

#include <cmath>
#include <vector>

TEST(MathUtils, Clamp) {
    EXPECT_EQ(MathUtils::Clamp(5, 0, 10), 5);
    EXPECT_EQ(MathUtils::Clamp(-3, 0, 10), 0);
    EXPECT_EQ(MathUtils::Clamp(99, 0, 10), 10);
    EXPECT_NEAR(MathUtils::Clamp(1.5, 0.0, 1.0), 1.0, 1e-12);
}

TEST(MathUtils, Lerp) {
    EXPECT_NEAR(MathUtils::Lerp(0.0, 10.0, 0.0), 0.0, 1e-12);
    EXPECT_NEAR(MathUtils::Lerp(0.0, 10.0, 1.0), 10.0, 1e-12);
    EXPECT_NEAR(MathUtils::Lerp(0.0, 10.0, 0.25), 2.5, 1e-12);
    EXPECT_NEAR(MathUtils::Lerp(-4.0, 4.0, 0.5), 0.0, 1e-12);
}

TEST(MathUtils, SmoothStepIsMonotonicAndClamped) {
    EXPECT_NEAR(MathUtils::SmoothStep(0.0, 1.0, -1.0), 0.0, 1e-12);
    EXPECT_NEAR(MathUtils::SmoothStep(0.0, 1.0, 2.0), 1.0, 1e-12);
    EXPECT_NEAR(MathUtils::SmoothStep(0.0, 1.0, 0.5), 0.5, 1e-12);
    // Strictly increasing across the interior.
    EXPECT_LT(MathUtils::SmoothStep(0.0, 1.0, 0.25), MathUtils::SmoothStep(0.0, 1.0, 0.75));
}

TEST(MathUtils, Remap) {
    EXPECT_NEAR(MathUtils::Remap(5.0, 0.0, 10.0, 0.0, 100.0), 50.0, 1e-9);
    EXPECT_NEAR(MathUtils::Remap(0.0, 0.0, 10.0, -1.0, 1.0), -1.0, 1e-9);
    // Degenerate input range maps to outMin.
    EXPECT_NEAR(MathUtils::Remap(7.0, 3.0, 3.0, 9.0, 99.0), 9.0, 1e-9);
}

TEST(MathUtils, AngleConversions) {
    EXPECT_NEAR(MathUtils::ToRadians(180.0), M_PI, 1e-9);
    EXPECT_NEAR(MathUtils::ToDegrees(M_PI), 180.0, 1e-9);
    // Round-trip.
    EXPECT_NEAR(MathUtils::ToDegrees(MathUtils::ToRadians(45.0)), 45.0, 1e-9);
}

TEST(MathUtils, WrapRadiansIntoPrincipalRange) {
    // WrapRadians maps an angle into the principal range (-pi, pi].
    auto inRange = [](double x) { return x > -M_PI - 1e-9 && x <= M_PI + 1e-9; };
    EXPECT_TRUE(inRange(MathUtils::WrapRadians(3 * M_PI)));
    EXPECT_TRUE(inRange(MathUtils::WrapRadians(-3 * M_PI)));
    EXPECT_TRUE(inRange(MathUtils::WrapRadians(10.0)));
    // The wrapped angle is congruent to the input modulo 2*pi.
    for (double a : {0.3, 2.0, 5.0, -4.0, 100.0}) {
        double w = MathUtils::WrapRadians(a);
        EXPECT_NEAR(std::cos(w), std::cos(a), 1e-9);
        EXPECT_NEAR(std::sin(w), std::sin(a), 1e-9);
    }
}

TEST(MathUtils, FactorialAndBinomial) {
    EXPECT_EQ(MathUtils::Factorial(0), 1ull);
    EXPECT_EQ(MathUtils::Factorial(5), 120ull);
    EXPECT_EQ(MathUtils::Binomial(5, 2), 10ull);
    EXPECT_EQ(MathUtils::Binomial(6, 0), 1ull);
    EXPECT_EQ(MathUtils::Binomial(6, 7), 0ull);  // k > n
    // Symmetry C(n,k) == C(n,n-k).
    EXPECT_EQ(MathUtils::Binomial(10, 3), MathUtils::Binomial(10, 7));
}

TEST(MathUtils, GcdLcm) {
    EXPECT_EQ(MathUtils::GCD(12, 18), 6ll);
    EXPECT_EQ(MathUtils::GCD(17, 5), 1ll);
    EXPECT_EQ(MathUtils::LCM(4, 6), 12ll);
    EXPECT_EQ(MathUtils::LCM(0, 5), 0ll);
}

TEST(MathUtils, SolveQuadratic) {
    // x^2 - 1 = 0  ->  roots {1, -1}
    auto r = MathUtils::SolveQuadratic(1.0, 0.0, -1.0);
    ASSERT_EQ(r.size(), 2u);
    // Each returned root must actually satisfy the equation.
    for (double x : r) {
        EXPECT_NEAR(1.0 * x * x + 0.0 * x - 1.0, 0.0, 1e-9);
    }

    // No real roots: x^2 + 1 = 0
    EXPECT_TRUE(MathUtils::SolveQuadratic(1.0, 0.0, 1.0).empty());

    // Linear degenerate: 2x - 4 = 0 -> x = 2
    auto lin = MathUtils::SolveQuadratic(0.0, 2.0, -4.0);
    ASSERT_EQ(lin.size(), 1u);
    EXPECT_NEAR(lin[0], 2.0, 1e-9);
}

RS2V_TEST_MAIN()
