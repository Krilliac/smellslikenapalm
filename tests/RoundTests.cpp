// tests/RoundTests.cpp
// Comprehensive unit tests for round and match lifecycle
//
// Covers:
// 1. Round start/end transitions.
// 2. Round timer expiration.
// 3. Score reset between rounds.
// 4. Round-based events: halftime, overtime.
// 5. Round count management.
// 6. Edge cases: zero-length rounds, concurrent end calls.

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "Game/GameMode.h"
#include "Game/RoundManager.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using namespace std::chrono_literals;

// Minimal GameModeDefinition for round tests
static GameModeDefinition MakeRoundDef() {
    GameModeDefinition def;
    def.modeName = "TestMode";
    def.matchDuration = 0.0f;
    def.maxScore = 0;
    def.respawnTime = 0.0f;
    def.objectiveTypes = {};
    return def;
}

// Fixture for RoundManager tests
class RoundTests : public ::testing::Test {
protected:
    void SetUp() override {
        rm = std::make_unique<RoundManager>();
        rm->Initialize(3, 30); // 3 rounds, 30s per round
    }
    std::unique_ptr<RoundManager> rm;
};

// 1. Initial state
TEST_F(RoundTests, InitialState_RoundZero_NotStarted) {
    EXPECT_EQ(rm->GetCurrentRound(), 0);
    EXPECT_FALSE(rm->IsRoundActive());
}

// 2. Start round transitions
TEST_F(RoundTests, StartRound_ActivatesRound) {
    rm->StartNextRound();
    EXPECT_EQ(rm->GetCurrentRound(), 1);
    EXPECT_TRUE(rm->IsRoundActive());
}

// 3. End round transitions
TEST_F(RoundTests, EndRound_DeactivatesRound) {
    rm->StartNextRound();
    rm->EndCurrentRound();
    EXPECT_FALSE(rm->IsRoundActive());
    EXPECT_EQ(rm->GetRoundsCompleted(), 1);
}

// 4. Timer expiration ends round
TEST_F(RoundTests, RoundTimer_Expires_AutoEnd) {
    rm->StartNextRound();
    std::this_thread::sleep_for(31s); // exceeds 30s
    rm->Update();
    EXPECT_FALSE(rm->IsRoundActive());
    EXPECT_EQ(rm->GetCurrentRound(), 1);
}

// 5. Multiple rounds progression
TEST_F(RoundTests, MultipleRounds_ProgressCorrectly) {
    for (int i = 1; i <= 3; ++i) {
        rm->StartNextRound();
        EXPECT_EQ(rm->GetCurrentRound(), i);
        rm->EndCurrentRound();
    }
    EXPECT_EQ(rm->GetRoundsCompleted(), 3);
    EXPECT_FALSE(rm->StartNextRound()); // no more rounds
}

// 6. Score reset between rounds
TEST_F(RoundTests, ScoreReset_BetweenRounds) {
    rm->StartNextRound();
    rm->AddRoundScore(1, 50);
    EXPECT_EQ(rm->GetRoundScore(1), 50);
    rm->EndCurrentRound();
    rm->StartNextRound();
    EXPECT_EQ(rm->GetRoundScore(2), 0);
}

// 7. Halftime event at mid-round
TEST_F(RoundTests, HalftimeEvent_TriggeredAtHalf) {
    bool halftimeCalled = false;
    rm->SetHalftimeCallback([&]() { halftimeCalled = true; });
    rm->StartNextRound();
    std::this_thread::sleep_for(15s);
    rm->Update();
    EXPECT_TRUE(halftimeCalled);
}

// 8. Overtime if tied at end of final round
TEST_F(RoundTests, OvertimeTriggered_OnTie) {
    rm->StartNextRound(); rm->AddRoundScore(1, 100); rm->EndCurrentRound();
    rm->StartNextRound(); rm->AddRoundScore(2, 100); rm->EndCurrentRound();
    rm->StartNextRound(); rm->AddRoundScore(3, 100); rm->EndCurrentRound();
    bool overtime = rm->IsOvertime();
    EXPECT_TRUE(overtime);
}

// 9. Zero-length round handled
TEST_F(RoundTests, ZeroLengthRound_ImmediateEnd) {
    rm = std::make_unique<RoundManager>();
    rm->Initialize(1, 0); // zero seconds
    rm->StartNextRound();
    rm->Update();
    EXPECT_FALSE(rm->IsRoundActive());
    EXPECT_EQ(rm->GetRoundsCompleted(), 1);
}

// 10. Concurrent EndCurrentRound calls safe
TEST_F(RoundTests, ConcurrentEndCalls_NoCrash) {
    rm->StartNextRound();
    std::thread t1([&](){ rm->EndCurrentRound(); });
    std::thread t2([&](){ rm->EndCurrentRound(); });
    t1.join(); t2.join();
    EXPECT_FALSE(rm->IsRoundActive());
    EXPECT_EQ(rm->GetRoundsCompleted(), 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}