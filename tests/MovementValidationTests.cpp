// tests/MovementValidationTests.cpp
// Unit tests for player movement validation and anti-cheat checks

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cmath>
#include <vector>

// Include actual headers
#include "Game/GameMode.h"
#include "Game/PlayerManager.h"
#include "Physics/CollisionDetection.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using ::testing::_; using ::testing::Return; using ::testing::StrictMock;

// Mock PlayerManager to validate movement constraints
class MockPlayerManager : public PlayerManager {
public:
    MOCK_METHOD(Vector3, GetPlayerPosition, (uint32_t playerId), (const, override));
    MOCK_METHOD(bool, IsValidPlayerPosition, (uint32_t playerId, const Vector3& pos), (const, override));
    MOCK_METHOD(void, UpdatePlayerPosition, (uint32_t playerId, const Vector3& pos), (override));
    MOCK_METHOD(float, GetMaxPlayerSpeed, (), (const, override));
};

// Mock GameMode to test movement handling
class TestGameMode : public GameMode {
public:
    TestGameMode() : GameMode(nullptr, GameModeDefinition{"Test", "",0,0,0,false,0,{},{}}, false) {}
    bool HandlePlayerMovement(uint32_t playerId, const Vector3& newPos) {
        auto oldPos = playerManager->GetPlayerPosition(playerId);
        float dist = (newPos - oldPos).Length();
        float maxSpeed = playerManager->GetMaxPlayerSpeed() * (1.0f / GetTickRate());
        if (dist > maxSpeed + 1e-3f) {
            Logger::Warn("Speed hack detected: moved %.3f > %.3f", dist, maxSpeed);
            return false;
        }
        if (!playerManager->IsValidPlayerPosition(playerId, newPos)) {
            Logger::Warn("Invalid position: (%.2f,%.2f,%.2f)", newPos.x, newPos.y, newPos.z);
            return false;
        }
        playerManager->UpdatePlayerPosition(playerId, newPos);
        return true;
    }
    void SetPlayerManager(MockPlayerManager* pm) { playerManager = pm; }
};

// Fixture
class MovementValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        pm = std::make_unique<StrictMock<MockPlayerManager>>();
        gm = std::make_unique<TestGameMode>();
        gm->SetPlayerManager(pm.get());
        // Assume tick rate 60
        ON_CALL(*pm, GetMaxPlayerSpeed()).WillByDefault(Return(600.0f)); 
    }
    std::unique_ptr<MockPlayerManager> pm;
    std::unique_ptr<TestGameMode> gm;
};

// Valid movement within speed limit
TEST_F(MovementValidationTest, Movement_WithinSpeedLimit_Succeeds) {
    uint32_t id = 42;
    Vector3 oldPos{0,0,0};
    Vector3 newPos{5,0,0}; // within 600 units/sec => 10 units/tick
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    ON_CALL(*pm, IsValidPlayerPosition(id, newPos)).WillByDefault(Return(true));
    EXPECT_CALL(*pm, UpdatePlayerPosition(id, newPos)).Times(1);
    EXPECT_TRUE(gm->HandlePlayerMovement(id, newPos));
}

// Movement exceeding speed limit flagged
TEST_F(MovementValidationTest, Movement_ExceedsSpeedLimit_Fails) {
    uint32_t id = 43;
    Vector3 oldPos{0,0,0};
    Vector3 newPos{20,0,0}; // >10 units/tick
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    EXPECT_CALL(*pm, IsValidPlayerPosition(_, _)).Times(0);
    EXPECT_FALSE(gm->HandlePlayerMovement(id, newPos));
}

// Invalid position rejected
TEST_F(MovementValidationTest, Movement_InvalidPosition_Fails) {
    uint32_t id = 44;
    Vector3 oldPos{0,0,0};
    Vector3 newPos{5,0,0};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    ON_CALL(*pm, GetMaxPlayerSpeed()).WillByDefault(Return(600.0f));
    ON_CALL(*pm, IsValidPlayerPosition(id, newPos)).WillByDefault(Return(false));
    EXPECT_FALSE(gm->HandlePlayerMovement(id, newPos));
}

// Edge: zero movement always allowed
TEST_F(MovementValidationTest, Movement_ZeroDelta_Succeeds) {
    uint32_t id = 45;
    Vector3 pos{10,5,-3};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(pos));
    Vector3 newPos = pos;
    ON_CALL(*pm, IsValidPlayerPosition(id, newPos)).WillByDefault(Return(true));
    EXPECT_CALL(*pm, UpdatePlayerPosition(id, newPos)).Times(1);
    EXPECT_TRUE(gm->HandlePlayerMovement(id, newPos));
}

// Stress: random valid movements
TEST_F(MovementValidationTest, Movement_RandomWithinLimit_Succeeds) {
    uint32_t id = 46;
    Vector3 oldPos{100,0,100};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    ON_CALL(*pm, IsValidPlayerPosition(id, _)).WillByDefault(Return(true));
    for (int i=0;i<1000;i++){
        Vector3 delta{(rand()%21-10),0,(rand()%21-10)};
        Vector3 newPos = oldPos + delta;
        EXPECT_TRUE(gm->HandlePlayerMovement(id, newPos));
    }
}

// Movement with collision detection
TEST_F(MovementValidationTest, Movement_CollisionDetected_Fails) {
    uint32_t id = 47;
    Vector3 oldPos{0,0,0};
    Vector3 newPos{2,0,0};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    // Simulate collision via invalid position
    ON_CALL(*pm, IsValidPlayerPosition(id, newPos)).WillByDefault(Return(false));
    EXPECT_FALSE(gm->HandlePlayerMovement(id, newPos));
}

// Boundary: exactly at speed limit
TEST_F(MovementValidationTest, Movement_AtSpeedLimit_Succeeds) {
    uint32_t id = 48;
    Vector3 oldPos{0,0,0};
    float maxPerTick = pm->GetMaxPlayerSpeed()/gm->GetTickRate(); //600/60=10
    Vector3 newPos{maxPerTick,0,0};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    ON_CALL(*pm, IsValidPlayerPosition(id, newPos)).WillByDefault(Return(true));
    EXPECT_CALL(*pm, UpdatePlayerPosition(id, newPos)).Times(1);
    EXPECT_TRUE(gm->HandlePlayerMovement(id, newPos));
}

// Attempted teleport hack
TEST_F(MovementValidationTest, Movement_Teleportation_Fails) {
    uint32_t id = 49;
    Vector3 oldPos{0,0,0};
    Vector3 newPos{1000,0,1000};
    ON_CALL(*pm, GetPlayerPosition(id)).WillByDefault(Return(oldPos));
    EXPECT_FALSE(gm->HandlePlayerMovement(id, newPos));
}

int main(int argc, char** argv){
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}