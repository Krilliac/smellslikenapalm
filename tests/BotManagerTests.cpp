#include "TestFramework.h"

#include "Game/BotManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct BotManagerTestAccess {
    static void SetSimulationTime(BotManager& manager, double simulationTime) {
        manager.simulationTime_ = simulationTime;
    }
};

namespace {

BotManagerConfig TestConfig(std::size_t fillTarget = 1) {
    BotManagerConfig config;
    config.fillTargetPerTeam = fillTarget;
    config.maxBotsPerTeam = 8;
    config.fixedStepSeconds = 0.1f;
    config.moveSpeed = 2.0f;
    config.respawnDelaySeconds = 0.2f;
    config.maxHealth = 100.0f;
    config.captureWeight = 1.5f;
    config.objectiveArrivalTolerance = 0.01f;
    config.maxCatchUpSteps = 16;
    config.navigation.maxDirectRouteLength = 1000.0f;
    return config;
}

std::vector<BotSpawnSnapshot> TeamSpawns() {
    // Intentionally unsorted: the manager sorts snapshots before choosing.
    return {
        BotSpawnSnapshot{21, BotManager::kTeamTwo, Vector3(101.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{11, BotManager::kTeamOne, Vector3(1.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo, Vector3(100.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{10, BotManager::kTeamOne, Vector3(0.0f, 0.0f, 0.0f)},
    };
}

ParticipantId FindTeamBot(const BotManager& manager, std::uint8_t teamId) {
    for (const BotSnapshot& bot : manager.GetBots()) {
        if (bot.teamId == teamId) {
            return bot.id;
        }
    }
    return ParticipantId{};
}

} // namespace

TEST(BotManager, FillsBothTeamsWithTaggedStableIdsAndDeterministicSpawns) {
    BotManager manager(TestConfig(2));
    manager.SetEligibleSpawns(TeamSpawns());

    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(2));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamTwo), static_cast<std::size_t>(2));
    EXPECT_EQ(manager.CountAliveBots(BotManager::kTeamOne), static_cast<std::size_t>(2));
    EXPECT_EQ(manager.CountAliveBots(BotManager::kTeamTwo), static_cast<std::size_t>(2));

    const BotSnapshot* first = manager.FindBot(ParticipantId::Bot(1));
    const BotSnapshot* second = manager.FindBot(ParticipantId::Bot(2));
    const BotSnapshot* third = manager.FindBot(ParticipantId::Bot(3));
    const BotSnapshot* fourth = manager.FindBot(ParticipantId::Bot(4));
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(third != nullptr);
    ASSERT_TRUE(fourth != nullptr);
    EXPECT_EQ(first->position, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(second->position, Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(third->position, Vector3(100.0f, 0.0f, 0.0f));
    EXPECT_EQ(fourth->position, Vector3(101.0f, 0.0f, 0.0f));

    EXPECT_NE(ParticipantId::Human(1), ParticipantId::Bot(1));
    EXPECT_TRUE(manager.Roster().Find(ParticipantId::Human(1)) == nullptr);
    EXPECT_TRUE(manager.Roster().Find(ParticipantId::Bot(1)) != nullptr);
}

TEST(BotManager, MovesAtFixedSpeedAndContributesObjectiveCaptureWeight) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.SetActiveObjectives(
        {BotObjectiveSnapshot{7, Vector3(1.0f, 0.0f, 0.0f), 0.25f, true, true}});

    ASSERT_TRUE(manager.Update(0.5f));
    const BotSnapshot* attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->lifecycle, BotLifecycle::Alive);
    EXPECT_NEAR(attacker->position.x, 1.0f, 0.0001f);
    EXPECT_NEAR(manager.GetObjectiveCaptureWeight(7, BotManager::kTeamOne), 1.5f,
                0.0001f);

    const std::vector<ObjectiveOccupantSnapshot> occupants =
        manager.GetObjectiveOccupants();
    ASSERT_EQ(occupants.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(occupants[0].participantId, ParticipantId::Bot(1));
    EXPECT_FALSE(manager.Update(std::numeric_limits<float>::infinity()));
}

TEST(BotManager, CombatDeathAndRespawnAreEventedAndIdempotent) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents(); // Discard initial-spawn events.

    const ParticipantId teamOne = FindTeamBot(manager, BotManager::kTeamOne);
    const ParticipantId teamTwo = FindTeamBot(manager, BotManager::kTeamTwo);
    ASSERT_TRUE(teamOne.IsBot());
    ASSERT_TRUE(teamTwo.IsBot());

    int deathCallbacks = 0;
    int deathBatchCallbacks = 0;
    std::size_t deathBatchSize = 0;
    int respawnCallbacks = 0;
    manager.SetDeathCallback([&deathCallbacks](const BotDeathEvent&) { ++deathCallbacks; });
    manager.SetDeathBatchCallback(
        [&deathBatchCallbacks, &deathBatchSize](
            const std::vector<BotDeathEvent>& deaths) {
            ++deathBatchCallbacks;
            deathBatchSize = deaths.size();
        });
    manager.SetRespawnCallback(
        [&respawnCallbacks](const BotRespawnEvent& event) {
            if (!event.initialSpawn) {
                ++respawnCallbacks;
            }
        });

    bool fired = false;
    manager.SetCombatResolver(
        [&fired, teamOne, teamTwo](const std::vector<BotSnapshot>&, float) {
            if (fired) {
                return std::vector<BotCombatEvent>{};
            }
            fired = true;
            return std::vector<BotCombatEvent>{BotCombatEvent{teamOne, teamTwo, 125.0f}};
        });

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* victim = manager.FindBot(teamTwo);
    ASSERT_TRUE(victim != nullptr);
    EXPECT_EQ(victim->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(deathCallbacks, 1);
    EXPECT_EQ(deathBatchCallbacks, 1);
    EXPECT_EQ(deathBatchSize, static_cast<std::size_t>(1));
    EXPECT_FALSE(manager.KillBot(teamTwo, teamOne));
    EXPECT_EQ(deathCallbacks, 1);
    EXPECT_EQ(deathBatchCallbacks, 1);
    EXPECT_EQ(manager.ConsumeCombatEvents().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(manager.ConsumeDeathEvents().size(), static_cast<std::size_t>(1));

    ASSERT_TRUE(manager.Update(0.1f));
    victim = manager.FindBot(teamTwo);
    ASSERT_TRUE(victim != nullptr);
    EXPECT_EQ(victim->lifecycle, BotLifecycle::RespawnQueued);
    ASSERT_TRUE(manager.Update(0.1f));
    victim = manager.FindBot(teamTwo);
    ASSERT_TRUE(victim != nullptr);
    EXPECT_EQ(victim->lifecycle, BotLifecycle::Alive);
    EXPECT_EQ(respawnCallbacks, 1);

    const std::vector<BotRespawnEvent> respawns = manager.ConsumeRespawnEvents();
    ASSERT_EQ(respawns.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(respawns[0].initialSpawn);
}

TEST(BotManager, RespawnDelayRemainsPreciseAfterLongUptime) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents(); // Discard initial-spawn events.

    const ParticipantId victim =
        FindTeamBot(manager, BotManager::kTeamTwo);
    ASSERT_TRUE(victim.IsBot());

    // At 2^21 seconds a float's spacing is 0.25 seconds, so the former float
    // clock could neither advance by the 0.1-second fixed step nor reliably
    // reach a 0.2-second respawn deadline.
    constexpr double kLongUptimeSeconds = 2097152.0;
    BotManagerTestAccess::SetSimulationTime(manager, kLongUptimeSeconds);
    ASSERT_TRUE(manager.KillBot(victim));

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* queued = manager.FindBot(victim);
    ASSERT_TRUE(queued != nullptr);
    EXPECT_EQ(queued->lifecycle, BotLifecycle::RespawnQueued);

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* respawned = manager.FindBot(victim);
    ASSERT_TRUE(respawned != nullptr);
    EXPECT_EQ(respawned->lifecycle, BotLifecycle::Alive);
    EXPECT_TRUE(manager.GetSimulationTime() > kLongUptimeSeconds);

    const std::vector<BotRespawnEvent> respawns =
        manager.ConsumeRespawnEvents();
    ASSERT_EQ(respawns.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(respawns[0].initialSpawn);
}

TEST(BotManager, HumanCountsEvictHighestIdsAndRefillWithoutIdReuse) {
    BotManager manager(TestConfig(3));
    manager.SetEligibleSpawns(TeamSpawns());

    manager.SetHumanTeamCount(BotManager::kTeamOne, 2);
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(1));
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(1)) != nullptr);
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(2)) == nullptr);
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(3)) == nullptr);

    manager.SetHumanTeamCount(BotManager::kTeamOne, 0);
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(3));
    // Team two originally occupied IDs 4-6, so refilled team-one bots get 7-8.
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(7)) != nullptr);
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(8)) != nullptr);
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(2)) == nullptr);
}

TEST(BotManager, FillEvictionEmitsOrderedRemovalWithoutCombatDeath) {
    BotManager manager(TestConfig(3));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamOne, 2));
    const std::vector<BotRemovalEvent> removed =
        manager.ConsumeRemovalEvents();
    ASSERT_EQ(removed.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(removed[0].botId, ParticipantId::Bot(3));
    EXPECT_EQ(removed[1].botId, ParticipantId::Bot(2));
    for (const BotRemovalEvent& event : removed) {
        EXPECT_EQ(event.teamId, BotManager::kTeamOne);
        EXPECT_TRUE(event.wasAlive);
        EXPECT_TRUE(manager.Roster().Find(event.botId) == nullptr);
    }
    EXPECT_TRUE(manager.ConsumeRemovalEvents().empty());
    EXPECT_TRUE(manager.ConsumeDeathEvents().empty());

    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamOne, 0));
    EXPECT_TRUE(manager.ConsumeRemovalEvents().empty());
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(7)) != nullptr);
    EXPECT_TRUE(manager.FindBot(ParticipantId::Bot(8)) != nullptr);
}

TEST(BotManager, EvictingDeadBotDoesNotManufactureSecondDeath) {
    BotManager manager(TestConfig(3));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();
    const ParticipantId victim = ParticipantId::Bot(3);

    ASSERT_TRUE(manager.KillBot(victim, ParticipantId::Bot(4), false));
    ASSERT_EQ(manager.ConsumeDeathEvents().size(), static_cast<std::size_t>(1));
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamOne, 1));

    const std::vector<BotRemovalEvent> removed =
        manager.ConsumeRemovalEvents();
    ASSERT_EQ(removed.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(removed[0].botId, victim);
    EXPECT_EQ(removed[0].teamId, BotManager::kTeamOne);
    EXPECT_FALSE(removed[0].wasAlive);
    EXPECT_TRUE(manager.ConsumeDeathEvents().empty());
    EXPECT_TRUE(manager.Roster().Find(victim) == nullptr);
}

TEST(BotManager, FillTargetSetterIsBoundedAndReconcilesImmediately) {
    BotManager manager(TestConfig(1));

    ASSERT_TRUE(manager.SetFillTargetPerTeam(3));
    EXPECT_EQ(manager.GetConfig().fillTargetPerTeam, static_cast<std::size_t>(3));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(3));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamTwo), static_cast<std::size_t>(3));

    EXPECT_FALSE(manager.SetFillTargetPerTeam(9));
    EXPECT_FALSE(manager.SetFillTargetPerTeam(std::numeric_limits<std::size_t>::max()));
    EXPECT_EQ(manager.GetConfig().fillTargetPerTeam, static_cast<std::size_t>(3));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(3));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamTwo), static_cast<std::size_t>(3));

    ASSERT_TRUE(manager.SetFillTargetPerTeam(0));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(0));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamTwo), static_cast<std::size_t>(0));
}

TEST(BotManager, ConfigureRejectsFillTargetAboveMaximumWithoutMutation) {
    BotManager manager(TestConfig(2));
    const BotManagerConfig before = manager.GetConfig();
    BotManagerConfig invalid = before;
    invalid.fillTargetPerTeam = invalid.maxBotsPerTeam + 1;

    EXPECT_FALSE(manager.Configure(invalid));
    EXPECT_EQ(manager.GetConfig().fillTargetPerTeam, before.fillTargetPerTeam);
    EXPECT_EQ(manager.GetConfig().maxBotsPerTeam, before.maxBotsPerTeam);
    EXPECT_EQ(manager.CountBots(BotManager::kTeamOne), static_cast<std::size_t>(2));
    EXPECT_EQ(manager.CountBots(BotManager::kTeamTwo), static_cast<std::size_t>(2));
}

TEST(BotManager, ExternalHumanDamageIsNonlethalThenUsesSingleDeathTransition) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    const ParticipantId victim = FindTeamBot(manager, BotManager::kTeamTwo);
    const ParticipantId attacker = ParticipantId::Human(42);
    ASSERT_TRUE(victim.IsBot());

    int deathCallbacks = 0;
    manager.SetDeathCallback([&deathCallbacks](const BotDeathEvent&) {
        ++deathCallbacks;
    });

    EXPECT_FALSE(manager.ApplyCombatEvent(
        BotCombatEvent{attacker, victim, 35.0f}));
    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        victim, attacker, 35.0f, ExternalBotDamageAuthorization::Hostile));
    const BotSnapshot* snapshot = manager.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_EQ(snapshot->lifecycle, BotLifecycle::Alive);
    EXPECT_NEAR(snapshot->health, 65.0f, 0.0001f);
    EXPECT_EQ(snapshot->deathSequence, static_cast<std::uint32_t>(0));

    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        victim, attacker, 80.0f, ExternalBotDamageAuthorization::Hostile));
    snapshot = manager.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_EQ(snapshot->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_NEAR(snapshot->health, 0.0f, 0.0001f);
    EXPECT_EQ(snapshot->deathSequence, static_cast<std::uint32_t>(1));
    EXPECT_EQ(deathCallbacks, 1);

    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, 1.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_EQ(deathCallbacks, 1);
    EXPECT_EQ(manager.ConsumeCombatEvents().size(), static_cast<std::size_t>(2));
    const std::vector<BotDeathEvent> deaths = manager.ConsumeDeathEvents();
    ASSERT_EQ(deaths.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(deaths[0].victimId, victim);
    EXPECT_EQ(deaths[0].killerId, attacker);
    EXPECT_EQ(deaths[0].deathSequence, static_cast<std::uint32_t>(1));
}

TEST(BotManager, ExternalDamageRejectsInvalidInputsUnknownBotsAndDeadVictims) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    const ParticipantId victim = FindTeamBot(manager, BotManager::kTeamTwo);
    const ParticipantId attacker = ParticipantId::Human(7);
    ASSERT_TRUE(victim.IsBot());

    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        ParticipantId{}, attacker, 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        ParticipantId::Human(victim.value), attacker, 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        ParticipantId::Bot(9999), attacker, 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, ParticipantId{}, 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, ParticipantId::Human(0), 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim,
        ParticipantId{static_cast<ParticipantKind>(255), 8}, 1.0f,
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, 0.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, -1.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, std::numeric_limits<float>::infinity(),
        ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, std::numeric_limits<float>::quiet_NaN(),
        ExternalBotDamageAuthorization::Hostile));
    const std::array<ExternalBotDamageAuthorization, 3> invalidAuthorizations{{
        static_cast<ExternalBotDamageAuthorization>(4),
        static_cast<ExternalBotDamageAuthorization>(127),
        static_cast<ExternalBotDamageAuthorization>(255),
    }};
    for (const ExternalBotDamageAuthorization invalidAuthorization :
         invalidAuthorizations) {
        EXPECT_FALSE(manager.ApplyExternalDamageToBot(
            victim, attacker, 1.0f, invalidAuthorization));
    }
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, 1.0f,
        ExternalBotDamageAuthorization::FriendlyFireBlocked));

    const BotSnapshot* snapshot = manager.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_NEAR(snapshot->health, 100.0f, 0.0001f);
    EXPECT_TRUE(manager.ConsumeCombatEvents().empty());

    ASSERT_TRUE(manager.KillBot(victim, attacker, false));
    manager.ConsumeDeathEvents();
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, 1.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_TRUE(manager.ConsumeCombatEvents().empty());
}

TEST(BotManager, ExternalHumanFriendlyFireRequiresCallerAuthorization) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    const ParticipantId victim = FindTeamBot(manager, BotManager::kTeamOne);
    const ParticipantId attacker = ParticipantId::Human(23);
    ASSERT_TRUE(victim.IsBot());

    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, attacker, 10.0f,
        ExternalBotDamageAuthorization::FriendlyFireBlocked));
    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        victim, attacker, 10.0f,
        ExternalBotDamageAuthorization::FriendlyFireAuthorized));

    const BotSnapshot* snapshot = manager.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_NEAR(snapshot->health, 90.0f, 0.0001f);
    const std::vector<BotCombatEvent> events = manager.ConsumeCombatEvents();
    ASSERT_EQ(events.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(events[0].attackerId, attacker);
}

TEST(BotManager, ExternalBotDamageEnforcesRelationshipAndAttackerValidity) {
    BotManager manager(TestConfig(2));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    const ParticipantId teamOneA = ParticipantId::Bot(1);
    const ParticipantId teamOneB = ParticipantId::Bot(2);
    const ParticipantId teamTwo = ParticipantId::Bot(3);
    ASSERT_TRUE(manager.FindBot(teamOneA) != nullptr);
    ASSERT_TRUE(manager.FindBot(teamOneB) != nullptr);
    ASSERT_TRUE(manager.FindBot(teamTwo) != nullptr);

    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        teamOneB, teamOneA, 10.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        teamOneB, teamOneA, 10.0f,
        ExternalBotDamageAuthorization::FriendlyFireBlocked));
    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        teamOneB, teamOneA, 10.0f,
        ExternalBotDamageAuthorization::FriendlyFireAuthorized));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        teamTwo, teamOneA, 10.0f,
        ExternalBotDamageAuthorization::FriendlyFireAuthorized));
    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        teamTwo, teamOneA, 10.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        teamTwo, ParticipantId::Bot(9999), 10.0f,
        ExternalBotDamageAuthorization::Hostile));

    ASSERT_TRUE(manager.KillBot(teamOneA, teamTwo, false));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        teamTwo, teamOneA, 10.0f, ExternalBotDamageAuthorization::Hostile));

    const BotSnapshot* friendlyVictim = manager.FindBot(teamOneB);
    const BotSnapshot* hostileVictim = manager.FindBot(teamTwo);
    ASSERT_TRUE(friendlyVictim != nullptr);
    ASSERT_TRUE(hostileVictim != nullptr);
    EXPECT_NEAR(friendlyVictim->health, 90.0f, 0.0001f);
    EXPECT_NEAR(hostileVictim->health, 90.0f, 0.0001f);
}

TEST(BotManager, ExternalSelfDamageRequiresExplicitSelfAuthorization) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();

    const ParticipantId victim = FindTeamBot(manager, BotManager::kTeamOne);
    const ParticipantId enemy = FindTeamBot(manager, BotManager::kTeamTwo);
    ASSERT_TRUE(victim.IsBot());
    ASSERT_TRUE(enemy.IsBot());

    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, victim, 15.0f, ExternalBotDamageAuthorization::Hostile));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, victim, 15.0f,
        ExternalBotDamageAuthorization::FriendlyFireAuthorized));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, enemy, 15.0f,
        ExternalBotDamageAuthorization::SelfDamageAuthorized));
    EXPECT_FALSE(manager.ApplyExternalDamageToBot(
        victim, ParticipantId::Human(44), 15.0f,
        ExternalBotDamageAuthorization::SelfDamageAuthorized));
    ASSERT_TRUE(manager.ApplyExternalDamageToBot(
        victim, victim, 15.0f,
        ExternalBotDamageAuthorization::SelfDamageAuthorized));

    const BotSnapshot* snapshot = manager.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_NEAR(snapshot->health, 85.0f, 0.0001f);
    const std::vector<BotCombatEvent> events = manager.ConsumeCombatEvents();
    ASSERT_EQ(events.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(events[0].attackerId, victim);
    EXPECT_EQ(events[0].victimId, victim);
    EXPECT_NEAR(events[0].damage, 15.0f, 0.0001f);
}

TEST(BotManager, TransitConfigRequiresCompleteFiniteOrderedBoundaries) {
    BotManager manager(TestConfig(1));
    const BotManagerConfig original = manager.GetConfig();

    std::vector<BotManagerConfig> invalid;
    BotManagerConfig candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    invalid.push_back(candidate); // Missing both distance boundaries.

    candidate = original;
    candidate.transitRouteDistanceThreshold = 100.0f;
    invalid.push_back(candidate); // Missing transit speed and approach boundary.

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold = 100.0f;
    invalid.push_back(candidate); // Missing approach boundary.

    candidate = original;
    candidate.transitMoveSpeed = original.moveSpeed - 0.5f;
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate); // A transit leg may not be slower than infantry.

    candidate = original;
    candidate.transitMoveSpeed = original.moveSpeed;
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate); // A no-op transit speed is not a valid fast leg.

    candidate = original;
    candidate.transitMoveSpeed = std::numeric_limits<float>::infinity();
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate);

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold =
        std::numeric_limits<float>::quiet_NaN();
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate);

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance =
        std::numeric_limits<float>::quiet_NaN();
    invalid.push_back(candidate);

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance = original.objectiveArrivalTolerance;
    invalid.push_back(candidate); // Must leave a real infantry approach segment.

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold =
        original.navigation.maxDirectRouteLength;
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate); // No qualifying direct route could exist.

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold = 10.0f;
    candidate.transitApproachDistance = 10.0f;
    invalid.push_back(candidate); // The long-route boundary must precede approach.

    candidate = original;
    candidate.transitMoveSpeed = 10.0f;
    candidate.transitRouteDistanceThreshold = 100.0f;
    candidate.transitApproachDistance = 10.0f;
    candidate.allowDirectRouteFallback = false;
    invalid.push_back(candidate); // Transit is defined only for direct fallback.

    for (const BotManagerConfig& config : invalid) {
        EXPECT_FALSE(manager.Configure(config));
        EXPECT_FLOAT_EQ(manager.GetConfig().transitMoveSpeed,
                        original.transitMoveSpeed);
        EXPECT_FLOAT_EQ(manager.GetConfig().transitRouteDistanceThreshold,
                        original.transitRouteDistanceThreshold);
        EXPECT_FLOAT_EQ(manager.GetConfig().transitApproachDistance,
                        original.transitApproachDistance);
    }

    BotManagerConfig valid = original;
    valid.transitMoveSpeed = original.moveSpeed + 1.0f;
    valid.transitRouteDistanceThreshold = 100.0f;
    valid.transitApproachDistance = original.objectiveArrivalTolerance + 1.0f;
    EXPECT_TRUE(manager.Configure(valid));
}

TEST(BotManager, LongDirectTransitStopsAtBoundaryThenUsesInfantrySpeed) {
    BotManagerConfig config = TestConfig(1);
    config.fixedStepSeconds = 1.0f;
    config.moveSpeed = 100.0f;
    config.transitMoveSpeed = 1000.0f;
    config.transitRouteDistanceThreshold = 1000.0f;
    config.transitApproachDistance = 500.0f;
    config.objectiveArrivalTolerance = 0.01f;
    config.maxCatchUpSteps = 4;
    config.navigation.maxDirectRouteLength = 5000.0f;

    const std::vector<BotSpawnSnapshot> spawns{
        BotSpawnSnapshot{10, BotManager::kTeamOne, Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo, Vector3(2000.0f, 0.0f, 0.0f)},
    };
    const BotObjectiveSnapshot objective{
        7, Vector3(2000.0f, 0.0f, 0.0f), 50.0f, true, true};

    BotManager manager(config);
    manager.SetEligibleSpawns(spawns);
    manager.SetActiveObjectives({objective});

    ASSERT_TRUE(manager.Update(1.0f));
    const BotSnapshot* attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_NEAR(attacker->position.x, 1000.0f, 0.001f);

    ASSERT_TRUE(manager.Update(1.0f));
    attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    // The fast leg covers only the 500 UU to the boundary (0.5 seconds), then
    // the remaining half-second advances 50 UU at infantry speed.
    EXPECT_NEAR(attacker->position.x, 1550.0f, 0.001f);

    ASSERT_TRUE(manager.Update(1.0f));
    attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_NEAR(attacker->position.x, 1650.0f, 0.001f);

    // Authored navigation must remain infantry-paced even when its endpoints
    // happen to be farther apart than the direct-route qualification boundary.
    BotManager authored(config);
    ASSERT_TRUE(authored.Navigation().AddWaypoint(
        1, Vector3(0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(authored.Navigation().AddWaypoint(
        2, Vector3(2000.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(authored.Navigation().ConnectWaypoints(1, 2));
    authored.SetEligibleSpawns(spawns);
    authored.SetActiveObjectives({objective});
    ASSERT_TRUE(authored.Update(1.0f));
    attacker = authored.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_NEAR(attacker->position.x, 100.0f, 0.001f);

    // A short direct route also remains infantry-paced.
    BotManager shortDirect(config);
    shortDirect.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(400.0f, 0.0f, 0.0f)},
    });
    shortDirect.SetActiveObjectives({BotObjectiveSnapshot{
        8, Vector3(400.0f, 0.0f, 0.0f), 50.0f, true, true}});
    ASSERT_TRUE(shortDirect.Update(1.0f));
    attacker = shortDirect.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_NEAR(attacker->position.x, 100.0f, 0.001f);
}

TEST(BotManager, ResortScaleTransitReachesZoneWithoutSpeedingNormalDefenderRoute) {
    BotManagerConfig infantry = TestConfig(1);
    infantry.fixedStepSeconds = 0.1f;
    infantry.moveSpeed = 300.0f;
    infantry.objectiveArrivalTolerance = 100.0f;
    infantry.maxCatchUpSteps = 4;
    infantry.navigation.maxDirectRouteLength = 200000.0f;

    constexpr float kIslandDistanceUu = 80422.586f;
    constexpr float kCaptureRadiusUu = 1750.0f;
    const std::vector<BotSpawnSnapshot> resortPhaseZeroSpawns{
        BotSpawnSnapshot{301195, BotManager::kTeamOne,
                         Vector3(-kIslandDistanceUu, 0.0f, 0.0f)},
        BotSpawnSnapshot{301185, BotManager::kTeamTwo,
                         Vector3(18328.0f, 0.0f, 0.0f)},
    };
    const BotObjectiveSnapshot beach{
        1, Vector3(0.0f, 0.0f, 0.0f), kCaptureRadiusUu, true, true};

    BotManager footOnly(infantry);
    footOnly.SetEligibleSpawns(resortPhaseZeroSpawns);
    footOnly.SetActiveObjectives({beach});
    for (int step = 0; step < 300; ++step) {
        ASSERT_TRUE(footOnly.Update(0.1f));
    }
    EXPECT_FLOAT_EQ(
        footOnly.GetObjectiveCaptureWeight(1, BotManager::kTeamOne), 0.0f);

    BotManagerConfig insertion = infantry;
    insertion.transitMoveSpeed = 3000.0f;
    insertion.transitRouteDistanceThreshold = 40000.0f;
    insertion.transitApproachDistance = 1000.0f;
    BotManager withTransit(insertion);
    withTransit.SetEligibleSpawns(resortPhaseZeroSpawns);
    withTransit.SetActiveObjectives({beach});
    for (int step = 0; step < 300; ++step) {
        ASSERT_TRUE(withTransit.Update(0.1f));
    }

    const BotSnapshot* attacker =
        withTransit.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_TRUE(std::fabs(attacker->position.x) <= kCaptureRadiusUu);
    EXPECT_NEAR(withTransit.GetObjectiveCaptureWeight(
                    1, BotManager::kTeamOne),
                insertion.captureWeight, 0.0001f);

    const BotSnapshot* defender =
        withTransit.FindBot(ParticipantId::Bot(2));
    ASSERT_TRUE(defender != nullptr);
    // The 18,328 UU defender leg is below the 40,000 UU qualification boundary,
    // so it remains at the ordinary 300 UU/s infantry pace.
    EXPECT_NEAR(defender->position.x, 9328.0f, 0.1f);
}

TEST(BotManager, RoundRedeployWaitsForEligibilityAndHasNoDeathSideEffects) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns(TeamSpawns());
    manager.SetActiveObjectives({BotObjectiveSnapshot{
        7, Vector3(50.0f, 0.0f, 0.0f), 1.0f, true, true}});
    ASSERT_TRUE(manager.Update(0.5f));

    const ParticipantId teamOne = ParticipantId::Bot(1);
    const ParticipantId teamTwo = ParticipantId::Bot(2);
    const BotSnapshot* before = manager.FindBot(teamOne);
    ASSERT_TRUE(before != nullptr);
    EXPECT_EQ(before->objectiveId, static_cast<std::uint32_t>(7));
    EXPECT_TRUE(before->position != Vector3{});

    manager.ConsumeCombatEvents();
    manager.ConsumeDeathEvents();
    manager.ConsumeRespawnEvents();
    int deathCallbacks = 0;
    int respawnCallbacks = 0;
    bool firstRespawnSawOtherTeamQueued = false;
    manager.SetDeathCallback(
        [&deathCallbacks](const BotDeathEvent&) { ++deathCallbacks; });
    manager.SetRespawnCallback(
        [&](const BotRespawnEvent& event) {
            ++respawnCallbacks;
            if (event.teamId == BotManager::kTeamOne) {
                const BotSnapshot* other = manager.FindBot(teamTwo);
                firstRespawnSawOtherTeamQueued =
                    other != nullptr &&
                    other->lifecycle == BotLifecycle::RespawnQueued;
            }
        });

    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamOne, false));
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamTwo, false));
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{30, BotManager::kTeamOne,
                         Vector3(-25.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{40, BotManager::kTeamTwo,
                         Vector3(125.0f, 0.0f, 0.0f)},
    });

    manager.RedeployForRound();
    const BotSnapshot* resetOne = manager.FindBot(teamOne);
    const BotSnapshot* resetTwo = manager.FindBot(teamTwo);
    ASSERT_TRUE(resetOne != nullptr);
    ASSERT_TRUE(resetTwo != nullptr);
    EXPECT_EQ(resetOne->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(resetTwo->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(resetOne->position, Vector3{});
    EXPECT_EQ(resetTwo->position, Vector3{});
    EXPECT_FLOAT_EQ(resetOne->health, 0.0f);
    EXPECT_FLOAT_EQ(resetTwo->health, 0.0f);
    EXPECT_EQ(resetOne->objectiveId, static_cast<std::uint32_t>(0));
    EXPECT_EQ(resetTwo->objectiveId, static_cast<std::uint32_t>(0));
    EXPECT_EQ(resetOne->deathSequence, static_cast<std::uint32_t>(0));
    EXPECT_EQ(resetTwo->deathSequence, static_cast<std::uint32_t>(0));
    ASSERT_TRUE(manager.Roster().Find(teamOne) != nullptr);
    ASSERT_TRUE(manager.Roster().Find(teamTwo) != nullptr);
    EXPECT_FALSE(manager.Roster().Find(teamOne)->alive);
    EXPECT_FALSE(manager.Roster().Find(teamTwo)->alive);
    EXPECT_EQ(deathCallbacks, 0);
    EXPECT_EQ(respawnCallbacks, 0);
    EXPECT_TRUE(manager.ConsumeCombatEvents().empty());
    EXPECT_TRUE(manager.ConsumeDeathEvents().empty());
    EXPECT_TRUE(manager.ConsumeRespawnEvents().empty());

    ASSERT_TRUE(manager.Update(1.0f));
    EXPECT_EQ(manager.FindBot(teamOne)->lifecycle,
              BotLifecycle::RespawnQueued);
    EXPECT_EQ(manager.FindBot(teamTwo)->lifecycle,
              BotLifecycle::RespawnQueued);

    // An open phase gate alone is insufficient when that team has no eligible
    // spawn. Adding the spawn later must release the same queued bot.
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{30, BotManager::kTeamOne,
                         Vector3(-25.0f, 0.0f, 0.0f)},
    });
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamTwo, true));
    EXPECT_EQ(manager.FindBot(teamTwo)->lifecycle,
              BotLifecycle::RespawnQueued);
    EXPECT_EQ(respawnCallbacks, 0);

    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamOne, true));
    resetOne = manager.FindBot(teamOne);
    resetTwo = manager.FindBot(teamTwo);
    ASSERT_TRUE(resetOne != nullptr);
    ASSERT_TRUE(resetTwo != nullptr);
    EXPECT_EQ(resetOne->lifecycle, BotLifecycle::Alive);
    EXPECT_EQ(resetOne->position, Vector3(-25.0f, 0.0f, 0.0f));
    EXPECT_EQ(resetTwo->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_TRUE(firstRespawnSawOtherTeamQueued);
    EXPECT_EQ(deathCallbacks, 0);
    EXPECT_EQ(respawnCallbacks, 1);

    manager.SetEligibleSpawns({
        BotSpawnSnapshot{30, BotManager::kTeamOne,
                         Vector3(-25.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{40, BotManager::kTeamTwo,
                         Vector3(125.0f, 0.0f, 0.0f)},
    });
    resetTwo = manager.FindBot(teamTwo);
    ASSERT_TRUE(resetTwo != nullptr);
    EXPECT_EQ(resetTwo->lifecycle, BotLifecycle::Alive);
    EXPECT_EQ(resetTwo->position, Vector3(125.0f, 0.0f, 0.0f));
    EXPECT_EQ(deathCallbacks, 0);
    EXPECT_EQ(respawnCallbacks, 2);

    const std::vector<BotRespawnEvent> respawns =
        manager.ConsumeRespawnEvents();
    ASSERT_EQ(respawns.size(), static_cast<std::size_t>(2));
    EXPECT_FALSE(respawns[0].initialSpawn);
    EXPECT_FALSE(respawns[1].initialSpawn);
}

TEST(BotManager, TerritoryPhaseRedeployUsesCurrentSpawnsWithoutCombatDeaths) {
    BotManager manager(TestConfig(1));
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne, Vector3(-10.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo, Vector3(10.0f, 0.0f, 0.0f)},
    });
    manager.ConsumeRespawnEvents();
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamOne, false));
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamTwo, false));
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{30, BotManager::kTeamOne, Vector3(-25.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{40, BotManager::kTeamTwo, Vector3(25.0f, 0.0f, 0.0f)},
    });

    manager.RedeployForTerritoryPhase();
    EXPECT_TRUE(manager.ConsumeDeathEvents().empty());
    EXPECT_EQ(manager.FindBot(ParticipantId::Bot(1))->lifecycle,
              BotLifecycle::RespawnQueued);
    EXPECT_EQ(manager.FindBot(ParticipantId::Bot(2))->lifecycle,
              BotLifecycle::RespawnQueued);
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamOne, true));
    ASSERT_TRUE(manager.SetTeamRespawnAllowed(BotManager::kTeamTwo, true));
    EXPECT_EQ(manager.FindBot(ParticipantId::Bot(1))->position,
              Vector3(-25.0f, 0.0f, 0.0f));
    EXPECT_EQ(manager.FindBot(ParticipantId::Bot(2))->position,
              Vector3(25.0f, 0.0f, 0.0f));
    const std::vector<BotRespawnEvent> respawns = manager.ConsumeRespawnEvents();
    ASSERT_EQ(respawns.size(), static_cast<std::size_t>(2));
    EXPECT_FALSE(respawns[0].initialSpawn);
    EXPECT_FALSE(respawns[1].initialSpawn);
}

TEST(BotManager, TerritoryRoleMappingAndGroupedObjectivePolicySurviveFillChurn) {
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(
                  BotManager::kTeamOne, BotManager::kTeamOne),
              BotManager::kTeamOne);
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(
                  BotManager::kTeamTwo, BotManager::kTeamOne),
              BotManager::kTeamTwo);
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(
                  BotManager::kTeamOne, BotManager::kTeamTwo),
              BotManager::kTeamTwo);
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(
                  BotManager::kTeamTwo, BotManager::kTeamTwo),
              BotManager::kTeamOne);
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(0, BotManager::kTeamOne),
              static_cast<std::uint8_t>(0));
    EXPECT_EQ(BotManager::RemapTerritorySpawnTeam(BotManager::kTeamOne, 3),
              static_cast<std::uint8_t>(0));

    BotManagerConfig config = TestConfig(4);
    config.moveSpeed = 0.0f;
    BotManager manager(config);
    EXPECT_FALSE(manager.SetTerritoryAttackingTeam(3));
    ASSERT_TRUE(manager.SetTerritoryAttackingTeam(BotManager::kTeamOne));
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne, Vector3{}},
        BotSpawnSnapshot{20, BotManager::kTeamTwo, Vector3{}},
    });
    manager.SetActiveObjectives({
        BotObjectiveSnapshot{10, Vector3{}, 1.0f, true, true},
        BotObjectiveSnapshot{20, Vector3{}, 1.0f, true, true},
    });
    ASSERT_TRUE(manager.Update(0.1f));

    const auto countAssignments = [&manager](std::uint8_t teamId,
                                              std::uint32_t objectiveId) {
        const std::vector<BotSnapshot> bots = manager.GetBots();
        return std::count_if(
            bots.begin(), bots.end(),
            [teamId, objectiveId](const BotSnapshot& bot) {
                return bot.teamId == teamId && bot.objectiveId == objectiveId;
            });
    };
    // Attackers serialise the grouped phase; defenders cover both branches.
    EXPECT_EQ(countAssignments(BotManager::kTeamOne, 10),
              static_cast<std::ptrdiff_t>(4));
    EXPECT_EQ(countAssignments(BotManager::kTeamOne, 20),
              static_cast<std::ptrdiff_t>(0));
    EXPECT_EQ(countAssignments(BotManager::kTeamTwo, 10),
              static_cast<std::ptrdiff_t>(2));
    EXPECT_EQ(countAssignments(BotManager::kTeamTwo, 20),
              static_cast<std::ptrdiff_t>(2));

    // Removing the highest defender bot and refilling creates a sparse id set.
    // Team-local ordinals must keep the authored 2/2 coverage.
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 0));
    ASSERT_TRUE(manager.Update(0.1f));
    EXPECT_EQ(countAssignments(BotManager::kTeamTwo, 10),
              static_cast<std::ptrdiff_t>(2));
    EXPECT_EQ(countAssignments(BotManager::kTeamTwo, 20),
              static_cast<std::ptrdiff_t>(2));
}

TEST(BotManager, TerritoryAssaultCoordinationIsRoleSymmetricAndDamageNeutral) {
    const auto simulate = [](std::uint8_t attackingTeam) {
        BotManagerConfig config = TestConfig(4);
        config.moveSpeed = 0.0f;
        config.combatRangeUu = 100.0f;
        config.combatDamage = 25.0f;
        config.combatRoundsPerMinute = 600.0f;
        BotManager manager(config);
        EXPECT_TRUE(manager.SetTerritoryAttackingTeam(attackingTeam));
        manager.SetEligibleSpawns({
            BotSpawnSnapshot{10, BotManager::kTeamOne, Vector3{}},
            BotSpawnSnapshot{20, BotManager::kTeamTwo, Vector3{}},
        });

        // Exercise the team-local ready ordinal with a sparse attacker roster.
        EXPECT_TRUE(manager.SetHumanTeamCount(attackingTeam, 1));
        EXPECT_TRUE(manager.SetHumanTeamCount(attackingTeam, 0));
        manager.ConsumeCombatEvents();
        manager.ConsumeRespawnEvents();
        EXPECT_TRUE(manager.Update(0.1f));

        const std::uint8_t defendingTeam = attackingTeam == BotManager::kTeamOne
            ? BotManager::kTeamTwo : BotManager::kTeamOne;
        std::size_t queuedDefenders = 0;
        bool attackerDamageIsUniform = true;
        bool survivingDefendersAreUntouched = true;
        for (const BotSnapshot& bot : manager.GetBots()) {
            if (bot.teamId == attackingTeam) {
                attackerDamageIsUniform = attackerDamageIsUniform &&
                    bot.lifecycle == BotLifecycle::Alive && bot.health == 75.0f;
            } else if (bot.teamId == defendingTeam) {
                if (bot.lifecycle == BotLifecycle::RespawnQueued) {
                    ++queuedDefenders;
                } else {
                    survivingDefendersAreUntouched =
                        survivingDefendersAreUntouched && bot.health == 100.0f;
                }
            }
        }
        EXPECT_TRUE(attackerDamageIsUniform);
        EXPECT_TRUE(survivingDefendersAreUntouched);
        EXPECT_EQ(queuedDefenders, static_cast<std::size_t>(1));
        EXPECT_EQ(manager.ConsumeCombatEvents().size(),
                  static_cast<std::size_t>(8));
        return std::array<std::size_t, 2>{
            manager.CountAliveBots(attackingTeam),
            manager.CountAliveBots(defendingTeam)};
    };

    const auto teamOneAttacks = simulate(BotManager::kTeamOne);
    const auto teamTwoAttacks = simulate(BotManager::kTeamTwo);
    EXPECT_EQ(teamOneAttacks, teamTwoAttacks);
    EXPECT_EQ(teamOneAttacks[0], static_cast<std::size_t>(4));
    EXPECT_EQ(teamOneAttacks[1], static_cast<std::size_t>(3));
}

TEST(BotManager, BuiltInCombatConfigIsAllOrZeroAndRejectsNonFiniteValues) {
    BotManager manager(TestConfig(1));
    const BotManagerConfig original = manager.GetConfig();

    BotManagerConfig invalid = original;
    invalid.combatRangeUu = 2500.0f;
    EXPECT_FALSE(manager.Configure(invalid));
    invalid = original;
    invalid.combatDamage = 5.0f;
    EXPECT_FALSE(manager.Configure(invalid));
    invalid = original;
    invalid.combatRoundsPerMinute = 600.0f;
    EXPECT_FALSE(manager.Configure(invalid));
    invalid = original;
    invalid.combatRangeUu = std::numeric_limits<float>::infinity();
    invalid.combatDamage = 5.0f;
    invalid.combatRoundsPerMinute = 600.0f;
    EXPECT_FALSE(manager.Configure(invalid));
    invalid = original;
    invalid.humanCombatGeneration = 0;
    EXPECT_FALSE(manager.Configure(invalid));

    BotManagerConfig valid = original;
    valid.combatRangeUu = 2500.0f;
    valid.combatDamage = 5.0f;
    valid.combatRoundsPerMinute = 600.0f;
    EXPECT_TRUE(manager.Configure(valid));
}

TEST(BotManager, CoLocatedCombatDistributesTargetsByTeamLocalReadyOrdinal) {
    BotManagerConfig config = TestConfig(2);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    config.combatDamage = 10.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });

    // Reproduce normal human-fill churn: the highest team-one id is removed,
    // then a later refill creates a sparse roster (Bot 1 and Bot 5). Raw-id
    // modulo would assign both attackers to the same victim in a two-target tie.
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamOne, 1));
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamOne, 0));
    ASSERT_TRUE(manager.FindBot(ParticipantId::Bot(1)) != nullptr);
    ASSERT_TRUE(manager.FindBot(ParticipantId::Bot(2)) == nullptr);
    ASSERT_TRUE(manager.FindBot(ParticipantId::Bot(5)) != nullptr);
    manager.ConsumeRespawnEvents();

    ASSERT_TRUE(manager.Update(0.1f));
    for (std::uint32_t id : {1u, 3u, 4u, 5u}) {
        const BotSnapshot* bot = manager.FindBot(ParticipantId::Bot(id));
        ASSERT_TRUE(bot != nullptr);
        EXPECT_FLOAT_EQ(bot->health, 90.0f);
    }

    const std::vector<BotCombatEvent> events = manager.ConsumeCombatEvents();
    ASSERT_EQ(events.size(), static_cast<std::size_t>(4));
    for (std::uint32_t victimId : {1u, 3u, 4u, 5u}) {
        EXPECT_EQ(std::count_if(
                      events.begin(), events.end(),
                      [victimId](const BotCombatEvent& event) {
                          return event.victimId ==
                              ParticipantId::Bot(victimId);
                      }),
                  static_cast<std::ptrdiff_t>(1));
    }
}

TEST(BotManager, BuiltInCombatResolvesBothSidesOfSimultaneousLethalVolley) {
    BotManagerConfig config = TestConfig(1);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    config.combatDamage = 100.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    manager.ConsumeRespawnEvents();

    int deathCallbacks = 0;
    bool callbackObservedPartialVolley = false;
    int deathBatchCallbacks = 0;
    std::size_t deathBatchSize = 0;
    bool batchObservedPartialVolley = false;
    manager.SetDeathCallback(
        [&manager, &deathCallbacks, &callbackObservedPartialVolley](
            const BotDeathEvent&) {
            ++deathCallbacks;
            const BotSnapshot* teamOne =
                manager.FindBot(ParticipantId::Bot(1));
            const BotSnapshot* teamTwo =
                manager.FindBot(ParticipantId::Bot(2));
            callbackObservedPartialVolley =
                callbackObservedPartialVolley || teamOne == nullptr ||
                teamTwo == nullptr ||
                teamOne->lifecycle == BotLifecycle::Alive ||
                teamTwo->lifecycle == BotLifecycle::Alive;
        });
    manager.SetDeathBatchCallback(
        [&manager, &deathBatchCallbacks, &deathBatchSize,
         &batchObservedPartialVolley](
            const std::vector<BotDeathEvent>& deaths) {
            ++deathBatchCallbacks;
            deathBatchSize = deaths.size();
            const BotSnapshot* teamOne =
                manager.FindBot(ParticipantId::Bot(1));
            const BotSnapshot* teamTwo =
                manager.FindBot(ParticipantId::Bot(2));
            batchObservedPartialVolley = teamOne == nullptr ||
                teamTwo == nullptr ||
                teamOne->lifecycle == BotLifecycle::Alive ||
                teamTwo->lifecycle == BotLifecycle::Alive;
        });

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* teamOne = manager.FindBot(ParticipantId::Bot(1));
    const BotSnapshot* teamTwo = manager.FindBot(ParticipantId::Bot(2));
    ASSERT_TRUE(teamOne != nullptr);
    ASSERT_TRUE(teamTwo != nullptr);
    EXPECT_EQ(teamOne->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(teamTwo->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_FLOAT_EQ(teamOne->health, 0.0f);
    EXPECT_FLOAT_EQ(teamTwo->health, 0.0f);
    EXPECT_EQ(deathCallbacks, 2);
    EXPECT_FALSE(callbackObservedPartialVolley);
    EXPECT_EQ(deathBatchCallbacks, 1);
    EXPECT_EQ(deathBatchSize, static_cast<std::size_t>(2));
    EXPECT_FALSE(batchObservedPartialVolley);

    const std::vector<BotCombatEvent> combat = manager.ConsumeCombatEvents();
    ASSERT_EQ(combat.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(combat[0].attackerId, ParticipantId::Bot(1));
    EXPECT_EQ(combat[0].victimId, ParticipantId::Bot(2));
    EXPECT_EQ(combat[1].attackerId, ParticipantId::Bot(2));
    EXPECT_EQ(combat[1].victimId, ParticipantId::Bot(1));

    const std::vector<BotDeathEvent> deaths = manager.ConsumeDeathEvents();
    ASSERT_EQ(deaths.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(deaths[0].victimId, ParticipantId::Bot(1));
    EXPECT_EQ(deaths[0].killerId, ParticipantId::Bot(2));
    EXPECT_EQ(deaths[1].victimId, ParticipantId::Bot(2));
    EXPECT_EQ(deaths[1].killerId, ParticipantId::Bot(1));
}

TEST(BotManager, OverkillVolleyEmitsEveryShotAndOneVictimDeath) {
    BotManagerConfig config = TestConfig(2);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    config.combatDamage = 100.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));
    manager.ConsumeRespawnEvents();

    int victimDeathCallbacks = 0;
    manager.SetDeathCallback([&victimDeathCallbacks](const BotDeathEvent& event) {
        if (event.victimId == ParticipantId::Bot(3)) {
            ++victimDeathCallbacks;
        }
    });
    ASSERT_TRUE(manager.Update(0.1f));

    const std::vector<BotCombatEvent> combat = manager.ConsumeCombatEvents();
    ASSERT_EQ(combat.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(std::count_if(
                  combat.begin(), combat.end(),
                  [](const BotCombatEvent& event) {
                      return event.victimId == ParticipantId::Bot(3);
                  }),
              static_cast<std::ptrdiff_t>(2));
    EXPECT_EQ(victimDeathCallbacks, 1);

    const BotSnapshot* victim = manager.FindBot(ParticipantId::Bot(3));
    ASSERT_TRUE(victim != nullptr);
    EXPECT_EQ(victim->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(victim->deathSequence, static_cast<std::uint32_t>(1));
    const std::vector<BotDeathEvent> deaths = manager.ConsumeDeathEvents();
    EXPECT_EQ(std::count_if(
                  deaths.begin(), deaths.end(),
                  [](const BotDeathEvent& event) {
                      return event.victimId == ParticipantId::Bot(3);
                  }),
              static_cast<std::ptrdiff_t>(1));
}

TEST(BotManager, OverkillCreditUsesTheShotThatActuallyCrossesHealth) {
    BotManagerConfig config = TestConfig(4);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    // Three rounds leave a small positive remainder in exact double
    // accumulation. A time-comparison epsilon must not credit that third round
    // when the fourth round is the one that actually makes the volley lethal.
    config.combatDamage = 33.333332f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    // Leave four South attackers and one North victim.
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 3));
    manager.ConsumeRespawnEvents();

    ASSERT_TRUE(manager.Update(0.1f));
    const std::vector<BotDeathEvent> deaths = manager.ConsumeDeathEvents();
    const auto victimDeath = std::find_if(
        deaths.begin(), deaths.end(), [](const BotDeathEvent& event) {
            return event.victimId == ParticipantId::Bot(5);
        });
    ASSERT_TRUE(victimDeath != deaths.end());
    EXPECT_EQ(victimDeath->killerId, ParticipantId::Bot(4));
}

TEST(BotManager, BuiltInCombatOutcomeIsSymmetricWhenLowHealthTeamIsReversed) {
    const auto simulate = [](std::uint8_t lowHealthTeam) {
        BotManagerConfig config = TestConfig(1);
        config.moveSpeed = 0.0f;
        config.combatRangeUu = 100.0f;
        config.combatDamage = 10.0f;
        config.combatRoundsPerMinute = 600.0f;
        BotManager manager(config);
        manager.SetEligibleSpawns({
            BotSpawnSnapshot{10, BotManager::kTeamOne,
                             Vector3(0.0f, 0.0f, 0.0f)},
            BotSpawnSnapshot{20, BotManager::kTeamTwo,
                             Vector3(0.0f, 0.0f, 0.0f)},
        });
        manager.ConsumeRespawnEvents();

        const ParticipantId lowHealth = lowHealthTeam == BotManager::kTeamOne
            ? ParticipantId::Bot(1)
            : ParticipantId::Bot(2);
        EXPECT_TRUE(manager.ApplyExternalDamageToBot(
            lowHealth, ParticipantId::Human(99), 90.0f,
            ExternalBotDamageAuthorization::Hostile));
        manager.ConsumeCombatEvents();
        EXPECT_TRUE(manager.Update(0.1f));

        const BotSnapshot* teamOne = manager.FindBot(ParticipantId::Bot(1));
        const BotSnapshot* teamTwo = manager.FindBot(ParticipantId::Bot(2));
        EXPECT_TRUE(teamOne != nullptr);
        EXPECT_TRUE(teamTwo != nullptr);
        if (teamOne == nullptr || teamTwo == nullptr) {
            return std::array<BotSnapshot, 2>{};
        }
        return std::array<BotSnapshot, 2>{{*teamOne, *teamTwo}};
    };

    const auto teamOneLow = simulate(BotManager::kTeamOne);
    const auto teamTwoLow = simulate(BotManager::kTeamTwo);
    EXPECT_EQ(teamOneLow[0].lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(teamOneLow[1].lifecycle, BotLifecycle::Alive);
    EXPECT_FLOAT_EQ(teamOneLow[1].health, 90.0f);
    EXPECT_EQ(teamTwoLow[0].lifecycle, BotLifecycle::Alive);
    EXPECT_FLOAT_EQ(teamTwoLow[0].health, 90.0f);
    EXPECT_EQ(teamTwoLow[1].lifecycle, BotLifecycle::RespawnQueued);
}

TEST(BotManager, ReadyBotHumanShotSurvivesSameVolleyBotDeath) {
    BotManagerConfig config = TestConfig(1);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    config.combatDamage = 100.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    manager.ConsumeRespawnEvents();
    ASSERT_TRUE(manager.SetExternalHumanCombatants({
        BotExternalHumanSnapshot{ParticipantId::Human(7),
            BotManager::kTeamOne, Vector3(0.0f, 0.0f, 0.0f), true},
    }));

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* teamTwo = manager.FindBot(ParticipantId::Bot(2));
    ASSERT_TRUE(teamTwo != nullptr);
    EXPECT_EQ(teamTwo->lifecycle, BotLifecycle::RespawnQueued);

    const std::vector<BotHumanCombatEvent> humanShots =
        manager.ConsumeHumanCombatEvents();
    ASSERT_EQ(humanShots.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(humanShots[0].attackerId, ParticipantId::Bot(2));
    EXPECT_EQ(humanShots[0].victimId, ParticipantId::Human(7));
    EXPECT_TRUE(manager.ResolveHumanCombatEvent(
        humanShots[0], BotHumanCombatOutcome::Rejected));
}

TEST(BotManager, CombatTargetDistributionNeverOverridesStrictNearestEnemy) {
    BotManagerConfig config = TestConfig(2);
    config.moveSpeed = 0.0f;
    config.combatRangeUu = 100.0f;
    config.combatDamage = 10.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns({
        BotSpawnSnapshot{10, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{20, BotManager::kTeamTwo,
                         Vector3(10.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{21, BotManager::kTeamTwo,
                         Vector3(20.0f, 0.0f, 0.0f)},
    });
    manager.ConsumeRespawnEvents();

    ASSERT_TRUE(manager.Update(0.1f));
    const BotSnapshot* nearer = manager.FindBot(ParticipantId::Bot(3));
    const BotSnapshot* farther = manager.FindBot(ParticipantId::Bot(4));
    ASSERT_TRUE(nearer != nullptr);
    ASSERT_TRUE(farther != nullptr);
    EXPECT_FLOAT_EQ(nearer->health, 80.0f);
    EXPECT_FLOAT_EQ(farther->health, 100.0f);
}

TEST(BotManager, HumanCombatViewRejectsMalformedDuplicateAndOversizedSnapshots) {
    BotManagerConfig config = TestConfig(1);
    config.combatRangeUu = 2500.0f;
    config.combatDamage = 5.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns(TeamSpawns());
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));

    const BotExternalHumanSnapshot valid{
        ParticipantId::Human(9), BotManager::kTeamTwo,
        Vector3(100.0f, 0.0f, 0.0f), true};
    EXPECT_FALSE(manager.SetExternalHumanCombatants({
        valid, valid}));
    ASSERT_TRUE(manager.Update(0.1f));
    EXPECT_TRUE(manager.ConsumeHumanCombatEvents().empty());

    BotExternalHumanSnapshot malformed = valid;
    malformed.id = ParticipantId::Bot(9);
    EXPECT_FALSE(manager.SetExternalHumanCombatants({malformed}));
    malformed = valid;
    malformed.position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(manager.SetExternalHumanCombatants({malformed}));

    std::vector<BotExternalHumanSnapshot> oversized;
    for (std::size_t i = 0; i <= BotManager::kMaxExternalHumans; ++i) {
        oversized.push_back(BotExternalHumanSnapshot{
            ParticipantId::Human(static_cast<std::uint32_t>(i + 1)),
            BotManager::kTeamTwo, Vector3(100.0f, 0.0f, 0.0f), true});
    }
    EXPECT_FALSE(manager.SetExternalHumanCombatants(oversized));
}

TEST(BotManager, BotPerceivesTaggedHumanAndViewExpiresAfterOneUpdate) {
    BotManagerConfig config = TestConfig(1);
    config.combatRangeUu = 2500.0f;
    config.combatDamage = 5.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns(TeamSpawns());
    manager.ConsumeRespawnEvents();
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));

    // Human(1) deliberately collides with Bot(1)'s raw value.
    ASSERT_TRUE(manager.SetExternalHumanCombatants({
        BotExternalHumanSnapshot{ParticipantId::Human(1),
            BotManager::kTeamTwo, Vector3(100.0f, 0.0f, 0.0f), true}}));
    ASSERT_TRUE(manager.Update(0.1f));
    const std::vector<BotHumanCombatEvent> shots =
        manager.ConsumeHumanCombatEvents();
    ASSERT_EQ(shots.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(shots[0].attackerId, ParticipantId::Bot(1));
    EXPECT_EQ(shots[0].victimId, ParticipantId::Human(1));
    EXPECT_NE(shots[0].attackerId, shots[0].victimId);
    EXPECT_TRUE(manager.IsPendingHumanCombatEvent(shots[0]));
    EXPECT_TRUE(manager.ResolveHumanCombatEvent(
        shots[0], BotHumanCombatOutcome::DamageApplied));

    // No refreshed human view means no stale target on the next ready shot.
    ASSERT_TRUE(manager.Update(0.1f));
    EXPECT_TRUE(manager.ConsumeHumanCombatEvents().empty());
}

TEST(BotManager, HumanShotCadenceAndResolutionAreExactAndIdempotent) {
    BotManagerConfig config = TestConfig(1);
    config.combatRangeUu = 2500.0f;
    config.combatDamage = 25.0f;
    config.combatRoundsPerMinute = 300.0f; // one shot every 0.2 seconds
    config.humanCombatGeneration = 17;
    BotManager manager(config);
    manager.SetEligibleSpawns(TeamSpawns());
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));
    const BotExternalHumanSnapshot target{
        ParticipantId::Human(42), BotManager::kTeamTwo,
        Vector3(100.0f, 0.0f, 0.0f), true};

    ASSERT_TRUE(manager.SetExternalHumanCombatants({target}));
    ASSERT_TRUE(manager.Update(0.1f));
    std::vector<BotHumanCombatEvent> shots =
        manager.ConsumeHumanCombatEvents();
    ASSERT_EQ(shots.size(), static_cast<std::size_t>(1));
    BotHumanCombatEvent forged = shots[0];
    EXPECT_EQ(shots[0].generation, static_cast<std::uint64_t>(17));
    forged.generation = 18;
    EXPECT_FALSE(manager.IsPendingHumanCombatEvent(forged));
    EXPECT_FALSE(manager.ResolveHumanCombatEvent(
        forged, BotHumanCombatOutcome::TargetKilled));
    forged = shots[0];
    forged.damage += 1.0f;
    EXPECT_FALSE(manager.IsPendingHumanCombatEvent(forged));
    EXPECT_TRUE(manager.ResolveHumanCombatEvent(
        shots[0], BotHumanCombatOutcome::TargetKilled));
    EXPECT_FALSE(manager.ResolveHumanCombatEvent(
        shots[0], BotHumanCombatOutcome::TargetKilled));
    const BotSnapshot* attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(1));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(1));

    ASSERT_TRUE(manager.SetExternalHumanCombatants({target}));
    ASSERT_TRUE(manager.Update(0.1f));
    EXPECT_TRUE(manager.ConsumeHumanCombatEvents().empty());
    ASSERT_TRUE(manager.SetExternalHumanCombatants({target}));
    ASSERT_TRUE(manager.Update(0.1f));
    shots = manager.ConsumeHumanCombatEvents();
    ASSERT_EQ(shots.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(manager.ResolveHumanCombatEvent(
        shots[0], BotHumanCombatOutcome::DamageApplied));
    attacker = manager.FindBot(ParticipantId::Bot(1));
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(1));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(1));
}

TEST(BotManager, HumanCombatSkipsFriendlyDeadUnknownAndOutOfRangeTargets) {
    BotManagerConfig config = TestConfig(1);
    config.combatRangeUu = 50.0f;
    config.combatDamage = 5.0f;
    config.combatRoundsPerMinute = 600.0f;
    BotManager manager(config);
    manager.SetEligibleSpawns(TeamSpawns());
    ASSERT_TRUE(manager.SetHumanTeamCount(BotManager::kTeamTwo, 1));

    ASSERT_TRUE(manager.SetExternalHumanCombatants({
        BotExternalHumanSnapshot{ParticipantId::Human(10),
            BotManager::kTeamOne, Vector3(10.0f, 0.0f, 0.0f), true},
        BotExternalHumanSnapshot{ParticipantId::Human(11),
            BotManager::kTeamTwo, Vector3(10.0f, 0.0f, 0.0f), false},
        BotExternalHumanSnapshot{ParticipantId::Human(12),
            BotManager::kTeamTwo, Vector3(1000.0f, 0.0f, 0.0f), true}}));
    ASSERT_TRUE(manager.Update(0.1f));
    EXPECT_TRUE(manager.ConsumeHumanCombatEvents().empty());
}

RS2V_TEST_MAIN()
