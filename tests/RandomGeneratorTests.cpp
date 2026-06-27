// tests/RandomGeneratorTests.cpp
//
// Native unit tests for src/Utils/RandomGenerator.{h,cpp} — the seedable PRNG
// facade. Asserts seed-determinism, range bounds, and degenerate-input
// hardening (min > max is normalized rather than UB).

#include "TestFramework.h"

#include "Utils/RandomGenerator.h"

#include <algorithm>
#include <vector>

TEST(RandomGenerator, SeedIsDeterministic) {
    auto& rng = Utils::RandomGenerator::Instance();

    rng.Seed(12345);
    std::vector<int32_t> first;
    for (int i = 0; i < 16; ++i) first.push_back(rng.Int(0, 1000000));

    rng.Seed(12345);
    std::vector<int32_t> second;
    for (int i = 0; i < 16; ++i) second.push_back(rng.Int(0, 1000000));

    EXPECT_EQ(first, second);  // same seed -> identical sequence
}

TEST(RandomGenerator, IntStaysInInclusiveRange) {
    auto& rng = Utils::RandomGenerator::Instance();
    rng.Seed(7);
    for (int i = 0; i < 1000; ++i) {
        int32_t v = rng.Int(-5, 5);
        EXPECT_GE(v, -5);
        EXPECT_LE(v, 5);
    }
    // A singleton range returns exactly that value.
    EXPECT_EQ(rng.Int(42, 42), 42);
}

TEST(RandomGenerator, InvertedRangeIsNormalized) {
    auto& rng = Utils::RandomGenerator::Instance();
    rng.Seed(99);
    for (int i = 0; i < 200; ++i) {
        int32_t v = rng.Int(10, 0);  // min > max: must not be UB
        EXPECT_GE(v, 0);
        EXPECT_LE(v, 10);
    }
}

TEST(RandomGenerator, RealStaysInRange) {
    auto& rng = Utils::RandomGenerator::Instance();
    rng.Seed(2024);
    for (int i = 0; i < 1000; ++i) {
        double v = rng.Real(0.0, 1.0);
        EXPECT_GE(v, 0.0);
        EXPECT_LT(v, 1.0);
    }
    for (int i = 0; i < 1000; ++i) {
        double v = rng.Real(-2.5, 2.5);
        EXPECT_GE(v, -2.5);
        EXPECT_LE(v, 2.5);
    }
}

TEST(RandomGenerator, ShufflePreservesElements) {
    auto& rng = Utils::RandomGenerator::Instance();
    rng.Seed(1);
    std::vector<int> v;
    for (int i = 0; i < 50; ++i) v.push_back(i);
    std::vector<int> original = v;

    rng.Shuffle(v.begin(), v.end());
    // Same multiset of elements, just (probably) reordered.
    std::sort(v.begin(), v.end());
    EXPECT_EQ(v, original);
}

RS2V_TEST_MAIN()
