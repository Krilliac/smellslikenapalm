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

#include <gtest/gtest.h>
#include "Math/Vector3.h"
#include <cmath>

static const float EPS = 1e-6f;

TEST(Vector3Test, DefaultConstructor_IsZero) {
    Math::Vector3 v;
    EXPECT_FLOAT_EQ(v.x, 0.0f);
    EXPECT_FLOAT_EQ(v.y, 0.0f);
    EXPECT_FLOAT_EQ(v.z, 0.0f);
}

TEST(Vector3Test, ValueConstructor_SetsComponents) {
    Math::Vector3 v(1.0f, -2.0f, 3.5f);
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, -2.0f);
    EXPECT_FLOAT_EQ(v.z, 3.5f);
}

TEST(Vector3Test, AdditionAndSubtraction_Works) {
    Math::Vector3 a(1,2,3), b(4,5,6);
    auto c = a + b;
    EXPECT_EQ(c, Math::Vector3(5,7,9));
    auto d = b - a;
    EXPECT_EQ(d, Math::Vector3(3,3,3));
}

TEST(Vector3Test, ScalarMultiplicationAndDivision) {
    Math::Vector3 v(2, -4, 6);
    auto m = v * 0.5f;
    EXPECT_EQ(m, Math::Vector3(1, -2, 3));
    auto d = v / 2.0f;
    EXPECT_EQ(d, Math::Vector3(1, -2, 3));
}

TEST(Vector3Test, LengthAndSquaredLength) {
    Math::Vector3 v(3,4,12);
    EXPECT_FLOAT_EQ(v.SquaredLength(), 169.0f);
    EXPECT_FLOAT_EQ(v.Length(), 13.0f);
}

TEST(Vector3Test, Normalize_UnitVector) {
    Math::Vector3 v(0,3,4);
    auto u = v.Normalized();
    EXPECT_NEAR(u.Length(), 1.0f, EPS);
    EXPECT_NEAR(u.x, 0.0f, EPS);
    EXPECT_NEAR(u.y, 0.6f, EPS);
    EXPECT_NEAR(u.z, 0.8f, EPS);
}

TEST(Vector3Test, DotProduct_CorrectValue) {
    Math::Vector3 a(1,2,3), b(4,-5,6);
    EXPECT_FLOAT_EQ(a.Dot(b), 12.0f);  // 1*4 + 2*-5 + 3*6 = 4 -10 +18 =12
}

TEST(Vector3Test, CrossProduct_RightHanded) {
    Math::Vector3 a(1,0,0), b(0,1,0);
    auto c = a.Cross(b);
    EXPECT_EQ(c, Math::Vector3(0,0,1));
}

TEST(Vector3Test, DistanceAndSquaredDistance) {
    Math::Vector3 a(1,2,3), b(4,6,3);
    EXPECT_FLOAT_EQ(a.SquaredDistance(b), 25.0f); // (3^2+4^2+0)
    EXPECT_FLOAT_EQ(a.Distance(b), 5.0f);
}

TEST(Vector3Test, Lerp_InterpolatesCorrectly) {
    Math::Vector3 a(0,0,0), b(10,0,0);
    auto m = Math::Vector3::Lerp(a, b, 0.25f);
    EXPECT_EQ(m, Math::Vector3(2.5f,0,0));
    m = Math::Vector3::Lerp(a, b, 1.5f);
    // beyond 1, unclamped
    EXPECT_EQ(m, Math::Vector3(15.0f,0,0));
}

TEST(Vector3Test, Clamp_ClampsComponents) {
    Math::Vector3 v(5,-5,10);
    auto min = Math::Vector3(0,0,0), max = Math::Vector3(3,3,3);
    auto c = Math::Vector3::Clamp(v, min, max);
    EXPECT_EQ(c, Math::Vector3(3,0,3));
}

TEST(Vector3Test, Edge_ZeroVectorNormalization) {
    Math::Vector3 v(0,0,0);
    auto u = v.Normalized();
    EXPECT_EQ(u, Math::Vector3(0,0,0));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}