#include "TestFramework.h"

#include "Game/BotManager.h"
#include "Game/ObjectiveSystem.h"
#include "Game/TeamMapping.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

CaptureZone MakeObjective(uint32_t id, uint32_t controllingTeam = 0) {
    CaptureZone zone;
    zone.id = id;
    zone.name = "Bot capture test";
    zone.controllingTeam = controllingTeam;
    zone.state = controllingTeam == 0 ? CaptureState::Neutral
                                      : CaptureState::Controlled;
    zone.captureSpeed = 0.10f;
    zone.isActive = true;
    zone.enabled = true;
    return zone;
}

bool RunResortBotRound(std::uint8_t attackingTeam, bool exerciseHumanFillChurn,
                       float& elapsedSeconds) {
    const std::uint8_t defendingTeam = attackingTeam == BotManager::kTeamOne
        ? BotManager::kTeamTwo : BotManager::kTeamOne;

    struct ObjectiveDefinition {
        std::uint32_t id;
        Vector3 position;
        int phase;
    };
    const std::array<ObjectiveDefinition, 5> objectiveDefinitions{{
        {1, Vector3(-11950.550f, 5238.709f, -544.1218f), 0},
        {2, Vector3(-3587.457f, 7545.831f, -80.9672f), 1},
        {3, Vector3(1517.340f, 3289.679f, 59.9254f), 2},
        {4, Vector3(1262.218f, 13525.040f, -20.8450f), 2},
        {5, Vector3(8320.872f, 8080.492f, 607.2889f), 3},
    }};

    ObjectiveSystem objectives(nullptr);
    for (const ObjectiveDefinition& definition : objectiveDefinitions) {
        CaptureZone zone = MakeObjective(definition.id, defendingTeam);
        zone.name = "Resort phase " + std::to_string(definition.phase);
        zone.type = ObjectiveType::Territory;
        zone.position = definition.position;
        zone.captureRadius = 35.0f;
        zone.territoryOrder = definition.phase;
        objectives.AddObjective(zone);
    }
    objectives.SetTerritoryOrder({1, 2, 3, 4, 5});
    objectives.ResetTerritoryForRound(defendingTeam);

    BotManagerConfig config;
    config.fillTargetPerTeam = 8;
    config.maxBotsPerTeam = 8;
    config.fixedStepSeconds = 0.1f;
    config.moveSpeed = 600.0f;
    config.transitMoveSpeed = 3000.0f;
    config.transitRouteDistanceThreshold = 40000.0f;
    config.transitApproachDistance = 1000.0f;
    config.respawnDelaySeconds = 5.0f;
    config.maxHealth = 100.0f;
    config.captureWeight = 1.0f;
    config.objectiveArrivalTolerance = 100.0f;
    config.maxCatchUpSteps = 8;
    config.allowDirectRouteFallback = true;
    config.combatRangeUu = 2500.0f;
    config.combatDamage = 5.0f;
    config.combatRoundsPerMinute = 600.0f;
    config.navigation.maxEdgeLength = 200000.0f;
    config.navigation.maxDirectRouteLength = 200000.0f;

    BotManager bots(config);
    if (!bots.SetTerritoryAttackingTeam(attackingTeam)) return false;
    objectives.SetBotCaptureWeightProvider(
        [&bots](std::uint32_t objectiveId, std::uint32_t teamId) {
            return bots.GetObjectiveCaptureWeight(
                objectiveId, static_cast<std::uint8_t>(teamId));
        });

    const auto authoredSpawnsForPhase = [](int phase) {
        struct AuthoredSpawn {
            Vector3 position;
            std::uint8_t teamId;
        };
        std::vector<AuthoredSpawn> spawns;
        switch (phase) {
            case 0:
                spawns = {
                    {Vector3(-92368.330f, 6117.922f, -491.0f), 1},
                    {Vector3(5339.0f, 3800.391f, 66.0f), 2},
                    {Vector3(5689.0f, 10170.0f, 67.0f), 2},
                };
                break;
            case 1:
                spawns = {
                    {Vector3(-8049.0f, 3739.0f, -433.8125f), 1},
                    {Vector3(-10431.820f, 15187.560f, -540.3362f), 1},
                    {Vector3(5689.0f, 10170.0f, 67.0f), 2},
                };
                break;
            case 2:
                spawns = {
                    {Vector3(-5959.0f, 7802.0f, -427.5624f), 1},
                    {Vector3(9625.0f, 7367.999f, 676.0f), 2},
                };
                break;
            case 3:
                spawns = {
                    {Vector3(1424.0f, 2696.0f, 70.0001f), 1},
                    {Vector3(1564.0f, 12478.0f, 15.0f), 1},
                    {Vector3(15803.0f, 9919.0f, 31.0f), 2},
                    {Vector3(16418.0f, 6338.0f, 137.0f), 2},
                };
                break;
            default:
                break;
        }
        return spawns;
    };

    std::vector<std::uint32_t> activeSignature;
    int deployedPhase = -1;
    bool churnStarted = false;
    int restoreBotsAtStep = -1;
    constexpr int kMaxSteps = 3000;
    for (int step = 0; step < kMaxSteps; ++step) {
        const std::vector<const CaptureZone*> active =
            objectives.GetActiveObjectives();
        std::vector<std::uint32_t> signature;
        std::vector<BotObjectiveSnapshot> botObjectives;
        int phase = -1;
        for (const CaptureZone* zone : active) {
            if (!zone) continue;
            signature.push_back(zone->id);
            botObjectives.push_back(BotObjectiveSnapshot{
                zone->id, zone->position,
                zone->captureRadius * ObjectiveSystem::kUnrealUnitsPerMeter,
                zone->isActive, zone->enabled});
            if (phase < 0) phase = zone->territoryOrder;
        }

        if (signature != activeSignature) {
            bots.SetTeamRespawnAllowed(BotManager::kTeamOne, false);
            bots.SetTeamRespawnAllowed(BotManager::kTeamTwo, false);
            std::vector<BotSpawnSnapshot> phaseSpawns;
            std::uint32_t spawnId = 1;
            for (const auto& authored : authoredSpawnsForPhase(phase)) {
                const std::uint8_t actualTeam =
                    BotManager::RemapTerritorySpawnTeam(
                        authored.teamId, attackingTeam);
                phaseSpawns.push_back(BotSpawnSnapshot{
                    spawnId++, actualTeam, authored.position});
            }
            bots.SetEligibleSpawns(phaseSpawns);
            bots.SetActiveObjectives(botObjectives);
            if (deployedPhase < 0) {
                bots.RedeployForRound();
            } else if (phase != deployedPhase) {
                bots.RedeployForTerritoryPhase();
            }
            bots.SetTeamRespawnAllowed(BotManager::kTeamOne, true);
            bots.SetTeamRespawnAllowed(BotManager::kTeamTwo, true);
            activeSignature = signature;
            deployedPhase = phase;
        }

        if (exerciseHumanFillChurn && phase == 2 && !churnStarted) {
            if (!bots.SetHumanTeamCount(attackingTeam, 1)) return false;
            churnStarted = true;
            restoreBotsAtStep = step + 10;
        }
        if (step == restoreBotsAtStep) {
            if (!bots.SetHumanTeamCount(attackingTeam, 0)) return false;
        }

        if (!bots.Update(0.1f)) return false;
        objectives.Update(0.1f, 0.1f);
        bots.ConsumeCombatEvents();
        bots.ConsumeDeathEvents();
        bots.ConsumeRespawnEvents();
        bots.ConsumeRemovalEvents();
        if (objectives.AreAllObjectivesCapturedBy(attackingTeam)) {
            elapsedSeconds = static_cast<float>(step + 1) * 0.1f;
            return !exerciseHumanFillChurn || churnStarted;
        }
    }

    elapsedSeconds = static_cast<float>(kMaxSteps) * 0.1f;
    return false;
}

} // namespace

TEST(ObjectiveSystemBotCapture, FractionalStrengthUsesContinuousHumanCurve) {
    CaptureZone zone = MakeObjective(1);

    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, /*team1CaptureWeight=*/1.5f,
                  /*team2CaptureWeight=*/0.0f, /*deltaSeconds=*/1.0f),
              0u);
    EXPECT_EQ(zone.state, CaptureState::Capturing);
    EXPECT_EQ(zone.cappingTeam, TeamMapping::kRetailUs);
    EXPECT_NEAR(zone.captureProgress, 0.125f, 0.00001f);
}

TEST(ObjectiveSystemBotCapture, ProviderCachesOnlyActiveFinitePositiveWeights) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeObjective(10));

    CaptureZone inactive = MakeObjective(11);
    inactive.isActive = false;
    objectives.AddObjective(inactive);

    objectives.SetBotCaptureWeightProvider(
        [](uint32_t objectiveId, uint32_t teamId) {
            if (objectiveId == 10 && teamId == TeamMapping::kServerUs) return 1.5f;
            if (objectiveId == 10 && teamId == TeamMapping::kServerNva) {
                return std::numeric_limits<float>::quiet_NaN();
            }
            return 7.0f;
        });
    objectives.RefreshPlayerZones();

    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(10, TeamMapping::kServerUs),
                    1.5f);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(10, TeamMapping::kServerNva),
                    0.0f);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(11, TeamMapping::kServerUs),
                    0.0f);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(10, 0), 0.0f);

    const CaptureZone* inactiveRuntime = objectives.GetObjective(11);
    ASSERT_NE(inactiveRuntime, nullptr);
    EXPECT_FLOAT_EQ(
        inactiveRuntime->botCaptureWeightByTeam[TeamMapping::kServerUs], 0.0f);
}

TEST(ObjectiveSystemBotCapture, InactivePhaseAndProviderDetachClearCachedWeights) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeObjective(20));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t, uint32_t) { return 2.0f; });
    objectives.RefreshPlayerZones();
    ASSERT_FLOAT_EQ(
        objectives.GetBotCaptureWeight(20, TeamMapping::kServerUs), 2.0f);

    CaptureZone* zone = objectives.GetObjective(20);
    ASSERT_NE(zone, nullptr);
    zone->isActive = false;
    objectives.RefreshPlayerZones();
    EXPECT_FLOAT_EQ(
        zone->botCaptureWeightByTeam[TeamMapping::kServerUs], 0.0f);
    EXPECT_FLOAT_EQ(
        zone->botCaptureWeightByTeam[TeamMapping::kServerNva], 0.0f);

    zone->isActive = true;
    objectives.RefreshPlayerZones();
    ASSERT_FLOAT_EQ(
        objectives.GetBotCaptureWeight(20, TeamMapping::kServerNva), 2.0f);
    objectives.SetBotCaptureWeightProvider({});
    EXPECT_FLOAT_EQ(
        objectives.GetBotCaptureWeight(20, TeamMapping::kServerNva), 0.0f);
}

TEST(ObjectiveSystemBotCapture, BotsCaptureControlledObjectiveWithoutHumanManager) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(
        MakeObjective(30, TeamMapping::kServerNva));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t objectiveId, uint32_t teamId) {
            return objectiveId == 30 && teamId == TeamMapping::kServerUs
                ? 1.5f
                : 0.0f;
        });

    objectives.Update(/*deltaSeconds=*/1.0f, /*captureDeltaSeconds=*/1.0f);

    const CaptureZone* zone = objectives.GetObjective(30);
    ASSERT_NE(zone, nullptr);
    EXPECT_EQ(zone->state, CaptureState::Capturing);
    EXPECT_EQ(zone->cappingTeam, TeamMapping::kRetailUs);
    EXPECT_NEAR(zone->captureProgress, 0.125f, 0.00001f);
}

TEST(ObjectiveSystemBotCapture, FractionalOppositionUsesNetCaptureStrength) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeObjective(40));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t objectiveId, uint32_t teamId) {
            if (objectiveId != 40) return 0.0f;
            return teamId == TeamMapping::kServerUs ? 1.5f : 0.5f;
        });

    objectives.Update(/*deltaSeconds=*/1.0f, /*captureDeltaSeconds=*/1.0f);

    const CaptureZone* zone = objectives.GetObjective(40);
    ASSERT_NE(zone, nullptr);
    EXPECT_EQ(zone->state, CaptureState::Capturing);
    EXPECT_EQ(zone->cappingTeam, TeamMapping::kRetailUs);
    EXPECT_NEAR(zone->captureProgress, 0.10f, 0.00001f);
}

TEST(ObjectiveSystemBotCapture,
     ResortTerritoryBotsCompleteEveryPhaseForEitherAttackingTeam) {
    for (const std::uint8_t attackingTeam :
         {BotManager::kTeamOne, BotManager::kTeamTwo}) {
        float elapsedSeconds = 0.0f;
        EXPECT_TRUE(RunResortBotRound(
            attackingTeam, /*exerciseHumanFillChurn=*/true, elapsedSeconds));
        // This is a liveness ceiling, not accelerated capture: authoritative
        // ObjectiveSystem weights/rates remain unchanged throughout the run.
        EXPECT_LT(elapsedSeconds, 300.0f);
    }
}

RS2V_TEST_MAIN()
