// tests/BoundsTests.cpp
//
// Native unit tests for src/Game/Bounds.h — the axis-aligned bounding box used
// by spawn/map geometry. Header-only struct over Math/Vector3.

#include "TestFramework.h"

#include "Game/Bounds.h"
#include "Math/Vector3.h"

TEST(Bounds, ContainsInteriorAndBoundary) {
    Bounds b{Vector3(0, 0, 0), Vector3(10, 10, 10)};
    EXPECT_TRUE(b.Contains(Vector3(5, 5, 5)));
    // Boundary points are inclusive (>= min, <= max).
    EXPECT_TRUE(b.Contains(Vector3(0, 0, 0)));
    EXPECT_TRUE(b.Contains(Vector3(10, 10, 10)));
}

TEST(Bounds, RejectsOutsidePoints) {
    Bounds b{Vector3(0, 0, 0), Vector3(10, 10, 10)};
    EXPECT_FALSE(b.Contains(Vector3(-0.1f, 5, 5)));
    EXPECT_FALSE(b.Contains(Vector3(5, 10.1f, 5)));
    EXPECT_FALSE(b.Contains(Vector3(5, 5, 20)));
}

TEST(Bounds, CenterAndSize) {
    Bounds b{Vector3(-2, 0, 4), Vector3(2, 6, 10)};
    Vector3 c = b.Center();
    EXPECT_FLOAT_EQ(c.x, 0.0f);
    EXPECT_FLOAT_EQ(c.y, 3.0f);
    EXPECT_FLOAT_EQ(c.z, 7.0f);

    Vector3 s = b.Size();
    EXPECT_FLOAT_EQ(s.x, 4.0f);
    EXPECT_FLOAT_EQ(s.y, 6.0f);
    EXPECT_FLOAT_EQ(s.z, 6.0f);
}

TEST(Bounds, DegenerateBoxContainsOnlyItsPoint) {
    Bounds point{Vector3(1, 1, 1), Vector3(1, 1, 1)};
    EXPECT_TRUE(point.Contains(Vector3(1, 1, 1)));
    EXPECT_FALSE(point.Contains(Vector3(1, 1, 1.001f)));
    Vector3 s = point.Size();
    EXPECT_FLOAT_EQ(s.x, 0.0f);
}

RS2V_TEST_MAIN()
