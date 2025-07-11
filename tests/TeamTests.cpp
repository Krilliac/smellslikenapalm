// tests/TeamTests.cpp
// Unit tests for team management and balancing
//
// Covers:
// 1. Team creation and player assignment.
// 2. Even distribution of players across teams.
// 3. Team score tracking and limits.
// 4. Auto-balancing on join/leave.
// 5. Team swap requests.
// 6. Edge cases: invalid team IDs, empty teams, concurrent assignments.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include "Game/TeamManager.h"
#include "Game/PlayerManager.h"
#include "Utils/Logger.h"

using ::testing::_; using ::testing::Return;

// Fixture
class TeamTests : public ::testing::Test {
protected:
    void SetUp() override {
        pm = std::make_unique<PlayerManager>();
        tm = std::make_unique<TeamManager>(pm.get());
        // Create two teams
        tm->CreateTeam(TeamID::NORTH_VIETNAM, "North");
        tm->CreateTeam(TeamID::SOUTH_VIETNAM, "South");
    }

    std::unique_ptr<PlayerManager> pm;
    std::unique_ptr<TeamManager> tm;
};

// 1. Player assignment to teams
TEST_F(TeamTests, AssignPlayerToTeam_Succeeds) {
    uint32_t pid = pm->AddNewPlayer("76561198000000001");
    EXPECT_TRUE(tm->AddPlayerToTeam(pid, TeamID::NORTH_VIETNAM));
    EXPECT_EQ(tm->GetPlayerTeam(pid), TeamID::NORTH_VIETNAM);
}

// 2. Even distribution on bulk join
TEST_F(TeamTests, BulkJoin_EvenDistribution) {
    const int N = 10;
    std::vector<uint32_t> pids;
    for (int i = 0; i < N; ++i) {
        pids.push_back(pm->AddNewPlayer("steam" + std::to_string(i)));
        tm->AddPlayerToBalancedTeam(pids.back());
    }
    int north = tm->GetTeamPlayerCount(TeamID::NORTH_VIETNAM);
    int south = tm->GetTeamPlayerCount(TeamID::SOUTH_VIETNAM);
    EXPECT_LE(std::abs(north - south), 1);
    EXPECT_EQ(north + south, N);
}

// 3. Team score tracking
TEST_F(TeamTests, TeamScore_IncrementAndReset) {
    tm->AddTeamScore(TeamID::NORTH_VIETNAM, 50);
    EXPECT_EQ(tm->GetTeamScore(TeamID::NORTH_VIETNAM), 50);
    tm->ResetTeamScores();
    EXPECT_EQ(tm->GetTeamScore(TeamID::NORTH_VIETNAM), 0);
}

// 4. Auto-balance on leave
TEST_F(TeamTests, AutoBalance_OnLeave) {
    uint32_t p1 = pm->AddNewPlayer("a");
    uint32_t p2 = pm->AddNewPlayer("b");
    uint32_t p3 = pm->AddNewPlayer("c");
    tm->AddPlayerToBalancedTeam(p1);
    tm->AddPlayerToBalancedTeam(p2);
    tm->AddPlayerToBalancedTeam(p3);
    // Remove one
    tm->RemovePlayerFromTeam(p2);
    // Remaining two should rebalance evenly
    int north = tm->GetTeamPlayerCount(TeamID::NORTH_VIETNAM);
    int south = tm->GetTeamPlayerCount(TeamID::SOUTH_VIETNAM);
    EXPECT_LE(std::abs(north - south), 1);
}

// 5. Team swap request
TEST_F(TeamTests, SwapTeams_Succeeds) {
    uint32_t pid = pm->AddNewPlayer("x");
    tm->AddPlayerToTeam(pid, TeamID::NORTH_VIETNAM);
    EXPECT_TRUE(tm->RequestTeamSwap(pid, TeamID::SOUTH_VIETNAM));
    EXPECT_EQ(tm->GetPlayerTeam(pid), TeamID::SOUTH_VIETNAM);
}

// 6. Invalid team or player IDs
TEST_F(TeamTests, InvalidIDs_HandledGracefully) {
    EXPECT_FALSE(tm->AddPlayerToTeam(9999, TeamID::NORTH_VIETNAM));
    uint32_t pid = pm->AddNewPlayer("y");
    EXPECT_FALSE(tm->AddPlayerToTeam(pid, TeamID::NONE));
    EXPECT_EQ(tm->GetPlayerTeam(12345), TeamID::NONE);
}

// 7. Empty teams scenario
TEST_F(TeamTests, EmptyTeams_StartEmpty) {
    EXPECT_EQ(tm->GetTeamPlayerCount(TeamID::NORTH_VIETNAM), 0);
    EXPECT_EQ(tm->GetTeamPlayerCount(TeamID::SOUTH_VIETNAM), 0);
}

// 8. Concurrent assignments
TEST_F(TeamTests, ConcurrentAssignments_NoRace) {
    const int THREADS = 4;
    const int PER_THREAD = 25;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                uint32_t pid = pm->AddNewPlayer("con" + std::to_string(t) + "_" + std::to_string(i));
                tm->AddPlayerToBalancedTeam(pid);
            }
        });
    }
    for (auto &th : threads) th.join();
    int total = tm->GetTeamPlayerCount(TeamID::NORTH_VIETNAM)
              + tm->GetTeamPlayerCount(TeamID::SOUTH_VIETNAM);
    EXPECT_EQ(total, THREADS * PER_THREAD);
}

// 9. Performance under many players
TEST_F(TeamTests, Performance_ManyPlayers_AssignmentFast) {
    const int N = 10000;
    std::vector<uint32_t> pids;
    pids.reserve(N);
    for (int i = 0; i < N; ++i) pids.push_back(pm->AddNewPlayer("p" + std::to_string(i)));
    auto start = std::chrono::high_resolution_clock::now();
    for (auto pid : pids) tm->AddPlayerToBalancedTeam(pid);
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(end - start).count();
    EXPECT_LT(ms, 200.0);  // assign 10k players <200ms
}

// 10. Edge: swapping to same team is no-op
TEST_F(TeamTests, SwapToSameTeam_NoOp) {
    uint32_t pid = pm->AddNewPlayer("same");
    tm->AddPlayerToTeam(pid, TeamID::NORTH_VIETNAM);
    EXPECT_TRUE(tm->RequestTeamSwap(pid, TeamID::NORTH_VIETNAM));
    EXPECT_EQ(tm->GetPlayerTeam(pid), TeamID::NORTH_VIETNAM);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
