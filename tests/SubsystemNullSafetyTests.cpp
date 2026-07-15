#include "TestFramework.h"

#include "Game/CommanderAbilities.h"
#include "Game/RoundManager.h"
#include "Game/ScoreManager.h"
#include "Game/TeamManager.h"

TEST(SubsystemNullSafety, ScoreManagerRetainsStateWithoutServer) {
    ScoreManager scores(nullptr);
    EXPECT_NO_THROW(scores.Initialize());
    EXPECT_NO_THROW(scores.RecordKill(1));
    EXPECT_EQ(scores.GetTeamScore(1).kills, 1u);
}

TEST(SubsystemNullSafety, TeamManagerRejectsDetachedPlayerAssignment) {
    TeamManager teams(nullptr);
    teams.Initialize();
    EXPECT_NO_THROW(teams.AddPlayerToTeam(7, 1));
    EXPECT_EQ(teams.GetPlayerTeam(7), 0u);
}

TEST(SubsystemNullSafety, RoundManagerUsesSafeDefaultsWithoutServer) {
    RoundManager rounds(nullptr);
    EXPECT_NO_THROW(rounds.Initialize());
    EXPECT_NO_THROW(rounds.StartRound());
    EXPECT_EQ(rounds.GetCurrentPhase(), RoundPhase::Active);
}

TEST(SubsystemNullSafety, CommanderAbilityRejectsDetachedRequestAndBadDelta) {
    CommanderAbilities abilities(nullptr);
    abilities.Initialize();
    EXPECT_FALSE(abilities.RequestAbility(
        1, AbilityType::Artillery, Vector3{1.0f, 2.0f, 3.0f}));
    EXPECT_NO_THROW(abilities.Update(-1.0f));
}

RS2V_TEST_MAIN()
