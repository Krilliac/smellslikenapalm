// tests/ObjectiveTests.cpp
// Unit tests for Objective capture, contesting, and win conditions
//
// Tests cover:
//  1. Objective initialization and defaults.
//  2. Single-team capture progress.
//  3. Contested capture blocking progress.
//  4. Capture completion awarding points.
//  5. Win condition when a team controls all objectives.
//  6. Decay of progress when zone is vacated.
//  7. Edge cases: zero objectives, invalid IDs.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>

#include "Game/GameMode.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using ::testing::_;

// Minimal GameModeDefinition for testing
static GameModeDefinition MakeConquestDef() {
    GameModeDefinition def;
    def.modeName = "Conquest";
    def.matchDuration = 300.0f;
    def.maxScore = 1000;
    def.respawnTime = 5.0f;
    def.objectiveTypes = { ObjectiveType::CAPTURE_POINT };
    return def;
}

// Test subclass exposing objectives
class TestGameMode : public GameMode {
public:
    TestGameMode() : GameMode(nullptr, MakeConquestDef(), false) { InitializeObjectives(); }

    using GameMode::m_objectives;
    using GameMode::m_teams;
    using GameMode::TriggerObjectiveCapture;
    using GameMode::m_gameState;
    using GameMode::UpdateObjectives;

    void InitializeObjectives() {
        m_objectives.clear();
        m_objectives.emplace_back(1, ObjectiveType::CAPTURE_POINT, "Alpha", Vector3(0,0,0));
        m_objectives.emplace_back(2, ObjectiveType::CAPTURE_POINT, "Bravo", Vector3(100,0,0));
        m_teams.clear();
        m_teams.emplace_back(TeamID::NORTH_VIETNAM,"North");
        m_teams.emplace_back(TeamID::SOUTH_VIETNAM,"South");
    }

    void SimulateCapture(int objId, TeamID team, float seconds) {
        for (int i=0;i<static_cast<int>(seconds*GetTickRate());++i) {
            // place one player from team in zone
            m_objectives[objId-1].playersInZone = { static_cast<uint32_t>(team) };
            UpdateObjectives(1.0f/GetTickRate());
        }
    }
};

class ObjectiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        gm = std::make_unique<TestGameMode>();
    }
    std::unique_ptr<TestGameMode> gm;
};

TEST_F(ObjectiveTest, Initialization_CreatesTwoObjectivesAndTwoTeams) {
    EXPECT_EQ(gm->m_objectives.size(), 2);
    EXPECT_EQ(gm->m_teams.size(), 2);
    EXPECT_EQ(gm->m_objectives[0].name, "Alpha");
    EXPECT_EQ(gm->m_objectives[1].name, "Bravo");
}

TEST_F(ObjectiveTest, SingleTeamCapture_ProgressIncreases) {
    auto& obj = gm->m_objectives[0];
    float before = obj.captureProgress;
    gm->SimulateCapture(1, TeamID::NORTH_VIETNAM, 1.0f); // 1 second
    EXPECT_GT(obj.captureProgress, before);
    EXPECT_EQ(obj.playersInZone.size(), 1u);
}

TEST_F(ObjectiveTest, ContestedCapture_NoProgress) {
    auto& obj = gm->m_objectives[0];
    float before = obj.captureProgress;
    // simulate alternating teams in zone each tick
    for (int i=0;i<gm->GetTickRate();++i) {
        obj.playersInZone = (i%2? std::vector<uint32_t>{1} : std::vector<uint32_t>{2});
        gm->UpdateObjectives(1.0f/gm->GetTickRate());
    }
    EXPECT_EQ(obj.captureProgress, before);
}

TEST_F(ObjectiveTest, CompleteCapture_ControllingTeamSetAndPointsAwarded) {
    auto& obj = gm->m_objectives[0];
    gm->SimulateCapture(1, TeamID::NORTH_VIETNAM, 5.0f); // enough time
    EXPECT_EQ(obj.captureProgress, 100.0f);
    EXPECT_EQ(obj.controllingTeam, TeamID::NORTH_VIETNAM);
    // team score increment
    auto& team = gm->m_teams[0];
    EXPECT_GT(team.teamScore, 0);
}

TEST_F(ObjectiveTest, AllObjectivesCaptured_TriggersRoundEnd) {
    // capture both
    gm->SimulateCapture(1, TeamID::SOUTH_VIETNAM, 5.0f);
    gm->SimulateCapture(2, TeamID::SOUTH_VIETNAM, 5.0f);
    // update win check
    gm->CheckWinConditions();
    EXPECT_EQ(gm->m_gameState, GameState::ROUND_END);
}

TEST_F(ObjectiveTest, ProgressDecay_WhenZoneEmpty) {
    auto& obj = gm->m_objectives[0];
    obj.captureProgress = 50.0f;
    obj.playersInZone.clear();
    gm->UpdateObjectives(1.0f);
    EXPECT_LT(obj.captureProgress, 50.0f);
}

TEST_F(ObjectiveTest, NoObjectives_HandleGracefully) {
    gm->m_objectives.clear();
    EXPECT_NO_THROW(gm->UpdateObjectives(1.0f));
    EXPECT_NO_THROW(gm->CheckWinConditions());
}

TEST_F(ObjectiveTest, InvalidObjectiveId_NoCrash) {
    EXPECT_NO_FATAL_FAILURE(gm->TriggerObjectiveCapture(999, TeamID::NORTH_VIETNAM));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}