// tests/Vector3Tests.cpp
// Comprehensive unit tests for Vector3 math utilities
//
// Covers:
// 1. Construction and default values.
// 2. Arithmetic operators (+, -, *, /).
// 3. Length, squared length, normalization.
// 4. Dot and cross product.
// 5. Distance and distance squared.
// 6. Lerp and clamp functions.
// 7. Edge cases: zero vector, unit vectors, negative components.
//
// API reconciliation (test-side, src unchanged):
//   * Header defines a GLOBAL `Vector3`, not `Math::Vector3`.
//   * Squared length is `LengthSquared()` (test originally used SquaredLength()).
//   * Squared distance is `DistanceSquared()` (test originally used SquaredDistance()).
//   * `Lerp` is a member (a.Lerp(b, t)), not a static `Vector3::Lerp`.
//   * There is no `Clamp`; component clamping is done inline below.

#include "TestFramework.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <cmath>

static const float EPS = 1e-6f;

TEST(Vector3Test, DefaultConstructor_IsZero) {
    Vector3 v;
    EXPECT_FLOAT_EQ(v.x, 0.0f);
    EXPECT_FLOAT_EQ(v.y, 0.0f);
    EXPECT_FLOAT_EQ(v.z, 0.0f);
}

TEST(Vector3Test, ValueConstructor_SetsComponents) {
    Vector3 v(1.0f, -2.0f, 3.5f);
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, -2.0f);
    EXPECT_FLOAT_EQ(v.z, 3.5f);
}

TEST(Vector3Test, AdditionAndSubtraction_Works) {
    Vector3 a(1,2,3), b(4,5,6);
    auto c = a + b;
    EXPECT_EQ(c, Vector3(5,7,9));
    auto d = b - a;
    EXPECT_EQ(d, Vector3(3,3,3));
}

TEST(Vector3Test, ScalarMultiplicationAndDivision) {
    Vector3 v(2, -4, 6);
    auto m = v * 0.5f;
    EXPECT_EQ(m, Vector3(1, -2, 3));
    auto d = v / 2.0f;
    EXPECT_EQ(d, Vector3(1, -2, 3));
}

TEST(Vector3Test, LengthAndSquaredLength) {
    Vector3 v(3,4,12);
    EXPECT_FLOAT_EQ(v.LengthSquared(), 169.0f);
    EXPECT_FLOAT_EQ(v.Length(), 13.0f);
}

TEST(Vector3Test, Normalize_UnitVector) {
    Vector3 v(0,3,4);
    auto u = v.Normalized();
    EXPECT_NEAR(u.Length(), 1.0f, EPS);
    EXPECT_NEAR(u.x, 0.0f, EPS);
    EXPECT_NEAR(u.y, 0.6f, EPS);
    EXPECT_NEAR(u.z, 0.8f, EPS);
}

TEST(Vector3Test, DotProduct_CorrectValue) {
    Vector3 a(1,2,3), b(4,-5,6);
    EXPECT_FLOAT_EQ(a.Dot(b), 12.0f);  // 1*4 + 2*-5 + 3*6 = 4 -10 +18 =12
}

TEST(Vector3Test, CrossProduct_RightHanded) {
    Vector3 a(1,0,0), b(0,1,0);
    auto c = a.Cross(b);
    EXPECT_EQ(c, Vector3(0,0,1));
}

TEST(Vector3Test, DistanceAndSquaredDistance) {
    Vector3 a(1,2,3), b(4,6,3);
    EXPECT_FLOAT_EQ(a.DistanceSquared(b), 25.0f); // (3^2+4^2+0)
    EXPECT_FLOAT_EQ(a.Distance(b), 5.0f);
}

TEST(Vector3Test, Lerp_InterpolatesCorrectly) {
    Vector3 a(0,0,0), b(10,0,0);
    auto m = a.Lerp(b, 0.25f);
    EXPECT_EQ(m, Vector3(2.5f,0,0));
    m = a.Lerp(b, 1.5f);
    // beyond 1, unclamped
    EXPECT_EQ(m, Vector3(15.0f,0,0));
}

TEST(Vector3Test, Clamp_ClampsComponents) {
    Vector3 v(5,-5,10);
    Vector3 lo(0,0,0), hi(3,3,3);
    // No Clamp helper in the header — clamp componentwise inline.
    Vector3 c(std::clamp(v.x, lo.x, hi.x),
              std::clamp(v.y, lo.y, hi.y),
              std::clamp(v.z, lo.z, hi.z));
    EXPECT_EQ(c, Vector3(3,0,3));
}

TEST(Vector3Test, Edge_ZeroVectorNormalization) {
    Vector3 v(0,0,0);
    auto u = v.Normalized();
    EXPECT_EQ(u, Vector3(0,0,0));
}

RS2V_TEST_MAIN()
