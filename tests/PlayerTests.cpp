// tests/PlayerTests.cpp
// Comprehensive unit tests for PlayerManager and player state management
//
// Covers:
// 1. Player add/remove lifecycle.
// 2. Health, ammo, inventory initialization and boundaries.
// 3. Respawn timing and state transitions.
// 4. Score tracking: kills, assists, deaths.
// 5. Inventory limits and usage.
// 6. Edge conditions: null player IDs, concurrent operations.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <chrono>
#include <thread>
#include <vector>

#include "Game/PlayerManager.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using ::testing::_;

// Fixture
class PlayerManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        pm = std::make_unique<PlayerManager>();
        playerId = pm->AddNewPlayer("76561198000000001");
    }

    void TearDown() override {
        pm.reset();
    }

    std::unique_ptr<PlayerManager> pm;
    uint32_t playerId;
};

// 1. Add and remove player
TEST_F(PlayerManagerTest, AddAndRemovePlayer_Succeeds) {
    EXPECT_NE(playerId, 0u);
    EXPECT_TRUE(pm->IsValidPlayer(playerId));
    pm->RemovePlayer(playerId);
    EXPECT_FALSE(pm->IsValidPlayer(playerId));
}

// 2. Initial state
TEST_F(PlayerManagerTest, InitialState_CorrectDefaults) {
    auto state = pm->GetPlayerState(playerId);
    EXPECT_EQ(state.health, pm->GetMaxHealth());
    EXPECT_EQ(state.ammo, pm->GetDefaultAmmo());
    EXPECT_EQ(state.score, 0);
    EXPECT_EQ(state.team, TeamID::NONE);
}

// 3. Health boundaries
TEST_F(PlayerManagerTest, HealthClamp_MinAndMax) {
    pm->SetHealth(playerId, pm->GetMaxHealth() + 50);
    EXPECT_EQ(pm->GetPlayerState(playerId).health, pm->GetMaxHealth());

    pm->SetHealth(playerId, -10);
    EXPECT_EQ(pm->GetPlayerState(playerId).health, 0);
}

// 4. Damage and death
TEST_F(PlayerManagerTest, ApplyDamage_TriggersDeath) {
    pm->SetHealth(playerId, 10);
    pm->ApplyDamage(playerId, 15, playerId);
    auto state = pm->GetPlayerState(playerId);
    EXPECT_EQ(state.health, 0);
    EXPECT_EQ(state.state, PlayerState::DEAD);
    EXPECT_EQ(pm->GetDeaths(playerId), 1);
}

// 5. Kill and assist scoring
TEST_F(PlayerManagerTest, RecordKillAndAssist_UpdatesScores) {
    uint32_t killer = playerId;
    uint32_t victim = pm->AddNewPlayer("76561198000000002");
    pm->ApplyDamage(victim, pm->GetMaxHealth(), killer);
    EXPECT_EQ(pm->GetKills(killer), 1);
    EXPECT_EQ(pm->GetScore(killer), pm->GetPointsPerKill());

    uint32_t assister = pm->AddNewPlayer("76561198000000003");
    pm->ApplyAssist(victim, assister);
    EXPECT_EQ(pm->GetAssists(assister), 1);
    EXPECT_EQ(pm->GetScore(assister), pm->GetPointsPerAssist());
}

// 6. Respawn timing
TEST_F(PlayerManagerTest, RespawnAfterDelay_StateTransitions) {
    pm->ApplyDamage(playerId, pm->GetMaxHealth(), playerId);
    EXPECT_EQ(pm->GetPlayerState(playerId).state, PlayerState::DEAD);
    std::this_thread::sleep_for(std::chrono::milliseconds(pm->GetRespawnTimeMs() + 50));
    pm->UpdateRespawns();
    EXPECT_EQ(pm->GetPlayerState(playerId).state, PlayerState::ALIVE);
    EXPECT_EQ(pm->GetPlayerState(playerId).health, pm->GetMaxHealth());
}

// 7. Inventory limits
TEST_F(PlayerManagerTest, Inventory_AddAndUseItems) {
    ItemID item = pm->GetDefaultItem();
    for (int i = 0; i < pm->GetMaxInventorySize(); ++i) {
        EXPECT_TRUE(pm->AddItem(playerId, item));
    }
    EXPECT_FALSE(pm->AddItem(playerId, item));  // inventory full

    EXPECT_TRUE(pm->UseItem(playerId, item));
    EXPECT_EQ(pm->GetInventoryCount(playerId), pm->GetMaxInventorySize() - 1);
}

// 8. Null and invalid IDs
TEST_F(PlayerManagerTest, InvalidPlayerID_HandledSafely) {
    EXPECT_FALSE(pm->IsValidPlayer(0));
    EXPECT_NO_THROW(pm->ApplyDamage(0, 10, playerId));
    EXPECT_NO_THROW(pm->SetHealth(0, 50));
    EXPECT_EQ(pm->GetScore(0), 0);
}

// 9. Concurrent add/remove
TEST_F(PlayerManagerTest, ConcurrentAddRemove_NoCrashes) {
    std::vector<std::thread> threads;
    std::atomic<bool> running{true};
    // Continuous add/remove
    threads.emplace_back([&](){
        while (running) {
            uint32_t id = pm->AddNewPlayer("temp");
            pm->RemovePlayer(id);
        }
    });
    // Run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;
    for (auto& t : threads) t.join();
    SUCCEED();  // no crash
}

// 10. Performance under churn
TEST_F(PlayerManagerTest, Performance_ManyPlayers_OperationsFast) {
    const int N = 10000;
    std::vector<uint32_t> ids;
    ids.reserve(N);
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) ids.push_back(pm->AddNewPlayer("p" + std::to_string(i)));
    for (auto id : ids) pm->RemovePlayer(id);
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    EXPECT_LT(ms, 50.0);  // add+remove 10k players <50ms
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}