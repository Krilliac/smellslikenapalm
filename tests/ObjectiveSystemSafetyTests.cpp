#include "TestFramework.h"

#include "Game/ObjectiveSystem.h"
#include "Game/TeamMapping.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

CaptureZone MakeZone(uint32_t id,
                     uint32_t controllingTeam = 0,
                     int territoryOrder = 0) {
    CaptureZone zone;
    zone.id = id;
    zone.name = "Objective safety test";
    zone.controllingTeam = controllingTeam;
    zone.state = controllingTeam == 0 ? CaptureState::Neutral
                                      : CaptureState::Controlled;
    zone.captureSpeed = 1.0f;
    zone.decaySpeed = 0.1f;
    zone.contestDecaySpeed = 0.05f;
    zone.territoryOrder = territoryOrder;
    zone.isActive = true;
    zone.enabled = true;
    return zone;
}

} // namespace

TEST(ObjectiveSystemSafety, NonFiniteProgressAndRatesFailClosed) {
    CaptureZone zone = MakeZone(1);
    zone.captureProgress = std::numeric_limits<float>::quiet_NaN();
    zone.captureSpeed = std::numeric_limits<float>::infinity();
    zone.decaySpeed = -4.0f;
    zone.cappingTeam = 72;

    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, 1.0f, 0.0f, 1.0f),
              0u);
    EXPECT_TRUE(std::isfinite(zone.captureProgress));
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.0f);
    EXPECT_EQ(zone.cappingTeam, TeamMapping::kRetailUs);

    zone.captureProgress = 0.5f;
    zone.decaySpeed = -10.0f;
    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, 0.0f, 0.0f, 1.0f),
              0u);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.5f);

    zone.captureProgress = std::numeric_limits<float>::infinity();
    zone.captureSpeed = 0.1f;
    EXPECT_EQ(ObjectiveSystem::ProcessNeutralCapture(
                  zone, 1.0f, 0.0f,
                  std::numeric_limits<float>::quiet_NaN()),
              0u);
    EXPECT_FLOAT_EQ(zone.captureProgress, 0.0f);
}

TEST(ObjectiveSystemSafety, UpdateRepairsExternallyMalformedAuthorityState) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(7));

    CaptureZone* zone = objectives.GetObjective(7);
    ASSERT_NE(zone, nullptr);
    zone->controllingTeam = 99;
    zone->captureProgress = std::numeric_limits<float>::quiet_NaN();
    zone->captureSpeed = -1.0f;
    zone->decaySpeed = -2.0f;
    zone->contestDecaySpeed = std::numeric_limits<float>::infinity();
    zone->state = static_cast<CaptureState>(255);
    zone->cappingTeam = 44;

    objectives.Update(1.0f, 1.0f);

    EXPECT_EQ(zone->controllingTeam, 0u);
    EXPECT_EQ(zone->state, CaptureState::Neutral);
    EXPECT_FLOAT_EQ(zone->captureProgress, 0.0f);
    EXPECT_FLOAT_EQ(zone->captureSpeed, 0.0f);
    EXPECT_FLOAT_EQ(zone->decaySpeed, 0.0f);
    EXPECT_FLOAT_EQ(zone->contestDecaySpeed, 0.0f);
    EXPECT_EQ(zone->cappingTeam, uint8_t{0xFF});
}

TEST(ObjectiveSystemSafety, ExtremeFiniteDistanceDoesNotOverflowInsideZone) {
    CaptureZone zone = MakeZone(1);
    const float maximum = std::numeric_limits<float>::max();
    zone.position = Vector3(-maximum / 2.0f, 0.0f, 0.0f);
    zone.captureRadius = maximum / 100.0f;
    const Vector3 distantPoint(maximum / 2.0f, 0.0f, 0.0f);

    EXPECT_FALSE(ObjectiveSystem::ContainsPoint2D(zone, distantPoint));
}

TEST(ObjectiveSystemSafety, RuntimeIdsNeverWrapOrOverwrite) {
    ObjectiveSystem objectives(nullptr);

    CaptureZone maximumId = MakeZone(std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(objectives.AddObjective(maximumId),
              std::numeric_limits<uint32_t>::max());

    CaptureZone automatic = MakeZone(0);
    const uint32_t automaticId = objectives.AddObjective(automatic);
    EXPECT_EQ(automaticId, 1u);

    CaptureZone duplicate = MakeZone(std::numeric_limits<uint32_t>::max());
    const uint32_t duplicateId = objectives.AddObjective(duplicate);
    EXPECT_EQ(duplicateId, 2u);
    EXPECT_EQ(objectives.GetObjectiveCount(), 3u);
    EXPECT_NE(objectives.GetObjective(automaticId), nullptr);
    EXPECT_NE(objectives.GetObjective(duplicateId), nullptr);
}

TEST(ObjectiveSystemSafety, InvalidTeamsAndTerritoryIdsFailClosed) {
    ObjectiveSystem objectives(nullptr);
    CaptureZone first = MakeZone(1, 99, 0);
    first.type = ObjectiveType::Territory;
    CaptureZone second = MakeZone(2, 0, 1);
    second.type = ObjectiveType::Territory;
    CaptureZone disabled = MakeZone(3, 0, 2);
    disabled.enabled = false;

    objectives.AddObjective(first);
    objectives.AddObjective(second);
    objectives.AddObjective(disabled);
    objectives.SetTerritoryOrder({999, 1, 1, 0, 3, 2});

    const CaptureZone* normalizedFirst = objectives.GetObjective(1);
    ASSERT_NE(normalizedFirst, nullptr);
    EXPECT_EQ(normalizedFirst->controllingTeam, 0u);
    ASSERT_NE(objectives.GetCurrentTerritoryObjective(), nullptr);
    EXPECT_EQ(objectives.GetCurrentTerritoryObjective()->id, 1u);
    EXPECT_EQ(objectives.GetTeamObjectiveCount(0), 0u);
    EXPECT_EQ(objectives.GetTeamObjectiveCount(99), 0u);
    EXPECT_FALSE(objectives.AreAllObjectivesCapturedBy(0));
    EXPECT_FALSE(objectives.AreAllObjectivesCapturedBy(99));

    objectives.ResetTerritoryForRound(99);
    EXPECT_EQ(objectives.GetObjective(1)->controllingTeam, 0u);

    objectives.ResetTerritoryForRound(TeamMapping::kServerNva);
    CaptureZone* runtimeFirst = objectives.GetObjective(1);
    CaptureZone* runtimeSecond = objectives.GetObjective(2);
    ASSERT_NE(runtimeFirst, nullptr);
    ASSERT_NE(runtimeSecond, nullptr);
    EXPECT_TRUE(runtimeFirst->isActive);
    EXPECT_FALSE(runtimeSecond->isActive);

    // The defender taking an objective cannot advance its own Territory chain.
    objectives.ActivateNextTerritory(TeamMapping::kServerNva);
    EXPECT_TRUE(runtimeFirst->isActive);
    EXPECT_FALSE(runtimeSecond->isActive);

    runtimeFirst->controllingTeam = TeamMapping::kServerUs;
    objectives.ActivateNextTerritory(TeamMapping::kServerUs);
    EXPECT_FALSE(runtimeFirst->isActive);
    EXPECT_TRUE(runtimeSecond->isActive);
}

TEST(ObjectiveSystemSafety, NonmonotonicTerritoryInputIsNormalizedByPhase) {
    ObjectiveSystem objectives(nullptr);
    CaptureZone later = MakeZone(10, TeamMapping::kServerNva, 1);
    later.type = ObjectiveType::Territory;
    CaptureZone first = MakeZone(20, TeamMapping::kServerNva, 0);
    first.type = ObjectiveType::Territory;
    objectives.AddObjective(later);
    objectives.AddObjective(first);

    objectives.SetTerritoryOrder({10, 20});
    ASSERT_NE(objectives.GetCurrentTerritoryObjective(), nullptr);
    EXPECT_EQ(objectives.GetCurrentTerritoryObjective()->id, 20u);
    EXPECT_TRUE(objectives.GetObjective(20)->isActive);
    EXPECT_FALSE(objectives.GetObjective(10)->isActive);

    objectives.GetObjective(20)->controllingTeam = TeamMapping::kServerUs;
    objectives.ActivateNextTerritory(TeamMapping::kServerUs);
    EXPECT_FALSE(objectives.GetObjective(20)->isActive);
    EXPECT_TRUE(objectives.GetObjective(10)->isActive);
}

TEST(ObjectiveSystemSafety, TerritoryOrderRejectsOtherTypesAndModeResetReactivatesThem) {
    ObjectiveSystem objectives(nullptr);
    CaptureZone capturePoint = MakeZone(40, TeamMapping::kServerUs, 0);
    capturePoint.type = ObjectiveType::CapturePoint;
    capturePoint.isActive = false;
    CaptureZone territory = MakeZone(41, TeamMapping::kServerNva, 1);
    territory.type = ObjectiveType::Territory;

    objectives.AddObjective(capturePoint);
    objectives.AddObjective(territory);
    objectives.SetTerritoryOrder({40, 41});

    ASSERT_NE(objectives.GetCurrentTerritoryObjective(), nullptr);
    EXPECT_EQ(objectives.GetCurrentTerritoryObjective()->id, 41u);
    EXPECT_FALSE(objectives.GetObjective(40)->isActive);

    objectives.ResetObjectivesToInitialOwners();
    EXPECT_TRUE(objectives.GetObjective(40)->isActive);
    EXPECT_EQ(objectives.GetObjective(40)->state, CaptureState::Controlled);
    EXPECT_EQ(objectives.GetObjective(40)->controllingTeam,
              TeamMapping::kServerUs);
}

TEST(ObjectiveSystemSafety, ReplacedProviderCannotPublishStaleWeights) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(10));

    int oldProviderCalls = 0;
    objectives.SetBotCaptureWeightProvider(
        [&](uint32_t, uint32_t) {
            ++oldProviderCalls;
            objectives.SetBotCaptureWeightProvider(
                [](uint32_t, uint32_t) { return 4.0f; });
            return 99.0f;
        });

    objectives.RefreshPlayerZones();
    EXPECT_EQ(oldProviderCalls, 1);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        10, TeamMapping::kServerUs),
                    0.0f);

    objectives.RefreshPlayerZones();
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        10, TeamMapping::kServerUs),
                    4.0f);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        10, TeamMapping::kServerNva),
                    4.0f);
}

TEST(ObjectiveSystemSafety, ProviderRecursionAndExceptionsAreContained) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(11));

    bool attemptedRecursion = false;
    objectives.SetBotCaptureWeightProvider(
        [&](uint32_t, uint32_t) {
            if (!attemptedRecursion) {
                attemptedRecursion = true;
                objectives.RefreshPlayerZones();
            }
            return 1.0f;
        });
    EXPECT_NO_THROW(objectives.RefreshPlayerZones());
    EXPECT_TRUE(attemptedRecursion);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        11, TeamMapping::kServerUs),
                    1.0f);

    objectives.SetBotCaptureWeightProvider(
        [](uint32_t, uint32_t) -> float { throw 17; });
    EXPECT_NO_THROW(objectives.RefreshPlayerZones());
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        11, TeamMapping::kServerUs),
                    0.0f);
    EXPECT_FLOAT_EQ(objectives.GetBotCaptureWeight(
                        11, TeamMapping::kServerNva),
                    0.0f);
}

TEST(ObjectiveSystemSafety, CaptureCallbackMayRemoveQueuedObjectiveSafely) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(1, TeamMapping::kServerNva));
    objectives.AddObjective(MakeZone(2, TeamMapping::kServerNva));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t, uint32_t teamId) {
            return teamId == TeamMapping::kServerUs ? 1.0f : 0.0f;
        });

    std::vector<uint32_t> capturedIds;
    objectives.SetOnObjectiveCaptured(
        [&](uint32_t objectiveId, uint32_t, uint32_t) {
            capturedIds.push_back(objectiveId);
            if (objectiveId == 1) {
                objectives.RemoveObjective(2);
                // A nested update is permitted after iteration, but cannot
                // recursively re-dispatch the callback currently on the stack.
                objectives.Update(0.0f, 0.0f);
            }
        });

    EXPECT_NO_THROW(objectives.Update(1.0f, 1.0f));
    ASSERT_EQ(capturedIds.size(), 1u);
    EXPECT_EQ(capturedIds.front(), 1u);
    EXPECT_EQ(objectives.GetObjective(2), nullptr);
}

TEST(ObjectiveSystemSafety, CaptureCallbackExceptionsDoNotBlockLaterEvents) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(21, TeamMapping::kServerNva));
    objectives.AddObjective(MakeZone(22, TeamMapping::kServerNva));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t, uint32_t teamId) {
            return teamId == TeamMapping::kServerUs ? 1.0f : 0.0f;
        });

    int callbackCount = 0;
    objectives.SetOnObjectiveCaptured(
        [&](uint32_t, uint32_t, uint32_t) {
            ++callbackCount;
            throw 23;
        });

    EXPECT_NO_THROW(objectives.Update(1.0f, 1.0f));
    EXPECT_EQ(callbackCount, 2);
    ASSERT_NE(objectives.GetObjective(21), nullptr);
    ASSERT_NE(objectives.GetObjective(22), nullptr);
    EXPECT_EQ(objectives.GetObjective(21)->controllingTeam,
              TeamMapping::kServerUs);
    EXPECT_EQ(objectives.GetObjective(22)->controllingTeam,
              TeamMapping::kServerUs);
}

TEST(ObjectiveSystemSafety, NestedCaptureUpdateQueuesWithoutRecursiveDispatch) {
    ObjectiveSystem objectives(nullptr);
    objectives.AddObjective(MakeZone(31, TeamMapping::kServerNva));
    objectives.SetBotCaptureWeightProvider(
        [](uint32_t, uint32_t teamId) {
            return teamId == TeamMapping::kServerUs ? 1.0f : 0.0f;
        });

    uint32_t callbackDepth = 0;
    uint32_t maximumDepth = 0;
    uint32_t callbackCount = 0;
    bool queuedNestedCapture = false;
    objectives.SetOnObjectiveCaptured(
        [&](uint32_t objectiveId, uint32_t, uint32_t) {
            ++callbackDepth;
            maximumDepth = std::max(maximumDepth, callbackDepth);
            ++callbackCount;
            if (!queuedNestedCapture) {
                queuedNestedCapture = true;
                CaptureZone* zone = objectives.GetObjective(objectiveId);
                ASSERT_NE(zone, nullptr);
                zone->controllingTeam = TeamMapping::kServerNva;
                zone->state = CaptureState::Controlled;
                zone->isActive = true;
                objectives.Update(1.0f, 1.0f);
            }
            --callbackDepth;
        });

    objectives.Update(1.0f, 1.0f);
    EXPECT_EQ(callbackCount, 2u);
    EXPECT_EQ(maximumDepth, 1u);
}

RS2V_TEST_MAIN()
