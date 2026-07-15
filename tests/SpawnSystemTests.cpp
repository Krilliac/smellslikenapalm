#include "TestFramework.h"

#include "Game/SpawnSystem.h"
#include "Game/TeamMapping.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace {

bool ContainsSpawn(const std::vector<const SpawnLocation*>& spawns,
                   uint32_t spawnId) {
    return std::any_of(
        spawns.begin(), spawns.end(),
        [spawnId](const SpawnLocation* spawn) {
            return spawn && spawn->id == spawnId;
        });
}

SpawnLocation MakeSpawn(SpawnType type, uint32_t teamId,
                        int minPhase = -1, int maxPhase = -1) {
    SpawnLocation spawn;
    spawn.type = type;
    spawn.name = "spawn";
    spawn.teamId = teamId;
    spawn.minTerritoryPhase = minPhase;
    spawn.maxTerritoryPhase = maxPhase;
    return spawn;
}

} // namespace

TEST(SpawnSystem, TerritoryBaseSpawnsFollowRoundRolesButRuntimeSpawnsDoNot) {
    SpawnAccessContext round;
    round.territoryActive = true;
    round.attackingTeam = TeamMapping::kServerUs;
    round.defendingTeam = TeamMapping::kServerNva;
    round.territoryPhase = 0;

    SpawnSystem spawns(
        nullptr,
        [&round](uint32_t playerId)
            -> std::optional<SpawnAccessContext> {
            SpawnAccessContext context = round;
            context.playerTeam = playerId == 101
                ? TeamMapping::kServerUs
                : (playerId == 202 ? TeamMapping::kServerNva : 0);
            return context;
        });
    const uint32_t attackerBase = spawns.AddSpawnLocation(
        MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerUs, 0, 0));
    const uint32_t defenderBase = spawns.AddSpawnLocation(
        MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerNva, 0, 0));
    const uint32_t usTunnel = spawns.AddSpawnLocation(
        MakeSpawn(SpawnType::Tunnel, TeamMapping::kServerUs));
    const uint32_t nvaForward = spawns.AddSpawnLocation(
        MakeSpawn(SpawnType::ForwardBase, TeamMapping::kServerNva));
    const uint32_t usHelicopter = spawns.AddSpawnLocation(
        MakeSpawn(SpawnType::Helicopter, TeamMapping::kServerUs));

    // Round one is byte-for-byte compatible with authored team ownership.
    auto teamOne = spawns.GetAvailableSpawns(101);
    auto teamTwo = spawns.GetAvailableSpawns(202);
    EXPECT_TRUE(ContainsSpawn(teamOne, attackerBase));
    EXPECT_FALSE(ContainsSpawn(teamOne, defenderBase));
    EXPECT_TRUE(ContainsSpawn(teamOne, usTunnel));
    EXPECT_TRUE(ContainsSpawn(teamOne, usHelicopter));
    EXPECT_TRUE(ContainsSpawn(teamTwo, defenderBase));
    EXPECT_TRUE(ContainsSpawn(teamTwo, nvaForward));
    ASSERT_TRUE(spawns.CanPlayerSpawnAt(101, attackerBase));
    EXPECT_TRUE(spawns.GetAvailableSpawns(303).empty());
    EXPECT_FALSE(spawns.CanPlayerSpawnAt(303, attackerBase));

    // Halftime: authored team-one BaseSpawn is still the attacker lane, now
    // owned by actual team two. Runtime spawns remain on their actual teams.
    round.attackingTeam = TeamMapping::kServerNva;
    round.defendingTeam = TeamMapping::kServerUs;
    teamOne = spawns.GetAvailableSpawns(101);
    teamTwo = spawns.GetAvailableSpawns(202);
    EXPECT_FALSE(ContainsSpawn(teamOne, attackerBase));
    EXPECT_TRUE(ContainsSpawn(teamOne, defenderBase));
    EXPECT_TRUE(ContainsSpawn(teamOne, usTunnel));
    EXPECT_TRUE(ContainsSpawn(teamOne, usHelicopter));
    EXPECT_TRUE(ContainsSpawn(teamTwo, attackerBase));
    EXPECT_FALSE(ContainsSpawn(teamTwo, defenderBase));
    EXPECT_TRUE(ContainsSpawn(teamTwo, nvaForward));

    // A deployment id advertised before SwitchSides is rejected at the final
    // commit predicate after roles change. SpawnPlayer calls this same check.
    EXPECT_FALSE(spawns.CanPlayerSpawnAt(101, attackerBase));
    EXPECT_FALSE(spawns.SpawnPlayer(101, attackerBase));

    // SquadLeader is also runtime-owned; its extra liveness/combat gate is not
    // part of this pure role/state check.
    const SpawnLocation squad =
        MakeSpawn(SpawnType::SquadLeader, TeamMapping::kServerUs);
    SpawnAccessContext teamOneContext = round;
    teamOneContext.playerTeam = TeamMapping::kServerUs;
    SpawnAccessContext teamTwoContext = round;
    teamTwoContext.playerTeam = TeamMapping::kServerNva;
    EXPECT_TRUE(SpawnSystem::IsSpawnEligibleForContext(
        squad, teamOneContext));
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(
        squad, teamTwoContext));
}

TEST(SpawnSystem, TerritoryAuthorizationPreservesPhaseStateAndFailsClosed) {
    SpawnLocation attacker =
        MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerUs, 1, 1);
    SpawnAccessContext context;
    context.playerTeam = TeamMapping::kServerNva;
    context.territoryActive = true;
    context.attackingTeam = TeamMapping::kServerNva;
    context.defendingTeam = TeamMapping::kServerUs;
    context.territoryPhase = 0;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    context.territoryPhase = 1;
    EXPECT_TRUE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    attacker.spawnCooldown = 1.0f;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    attacker.spawnCooldown = 0.0f;
    attacker.isDestroyed = true;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    attacker.isDestroyed = false;
    attacker.isActive = false;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    attacker.isActive = true;
    attacker.spawnCooldown = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));

    attacker.spawnCooldown = 0.0f;
    context.attackingTeam = TeamMapping::kServerUs;
    context.defendingTeam = TeamMapping::kServerUs;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    context.attackingTeam = 0;
    context.defendingTeam = TeamMapping::kServerUs;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));

    // Outside Territory, phase bounds and role values are irrelevant; actual
    // team ownership is unchanged.
    context = SpawnAccessContext{};
    context.playerTeam = TeamMapping::kServerUs;
    EXPECT_TRUE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));
    context.playerTeam = TeamMapping::kServerNva;
    EXPECT_FALSE(SpawnSystem::IsSpawnEligibleForContext(attacker, context));

    EXPECT_EQ(TeamMapping::ResolveTerritoryRoleTeam(1, 1, 2), 1u);
    EXPECT_EQ(TeamMapping::ResolveTerritoryRoleTeam(2, 1, 2), 2u);
    EXPECT_EQ(TeamMapping::ResolveTerritoryRoleTeam(1, 2, 1), 2u);
    EXPECT_EQ(TeamMapping::ResolveTerritoryRoleTeam(2, 2, 1), 1u);
    EXPECT_EQ(TeamMapping::ResolveTerritoryRoleTeam(1, 1, 1), 0u);
}

TEST(SpawnSystem, MissingOrMalformedAccessContextPublishesNoSpawns) {
    SpawnSystem detached(nullptr);
    const uint32_t base = detached.AddSpawnLocation(
        MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerUs));
    EXPECT_TRUE(detached.GetAvailableSpawns(101).empty());
    EXPECT_FALSE(detached.CanPlayerSpawnAt(101, base));

    SpawnSystem malformed(
        nullptr,
        [](uint32_t) -> std::optional<SpawnAccessContext> {
            SpawnAccessContext context;
            context.playerTeam = TeamMapping::kServerUs;
            context.territoryActive = true;
            context.attackingTeam = TeamMapping::kServerUs;
            context.defendingTeam = TeamMapping::kServerUs;
            context.territoryPhase = 0;
            return context;
        });
    const uint32_t malformedBase = malformed.AddSpawnLocation(
        MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerUs, 0, 0));
    EXPECT_TRUE(malformed.GetAvailableSpawns(101).empty());
    EXPECT_FALSE(malformed.CanPlayerSpawnAt(101, malformedBase));
}

TEST(SpawnSystem, SquadLeaderCommitValidatesTheSelectedLeaderNotAnyLeader) {
    const auto teamOneContext =
        [](uint32_t playerId) -> std::optional<SpawnAccessContext> {
            if (playerId != 101) return std::nullopt;
            SpawnAccessContext context;
            context.playerTeam = TeamMapping::kServerUs;
            return context;
        };
    SpawnSystem spawns(
        nullptr,
        teamOneContext,
        [](uint32_t playerId, const SpawnLocation& location) {
            return playerId == 101 && location.squadLeaderId == 22;
        });

    SpawnLocation stale =
        MakeSpawn(SpawnType::SquadLeader, TeamMapping::kServerUs);
    stale.squadLeaderId = 11;
    const uint32_t staleId = spawns.AddSpawnLocation(stale);

    SpawnLocation valid =
        MakeSpawn(SpawnType::SquadLeader, TeamMapping::kServerUs);
    valid.squadLeaderId = 22;
    const uint32_t validId = spawns.AddSpawnLocation(valid);

    const auto available = spawns.GetAvailableSpawns(101);
    EXPECT_FALSE(ContainsSpawn(available, staleId));
    EXPECT_TRUE(ContainsSpawn(available, validId));
    EXPECT_FALSE(spawns.CanPlayerSpawnAt(101, staleId));
    EXPECT_TRUE(spawns.CanPlayerSpawnAt(101, validId));

    // The injection is test-only. The production path requires both managers
    // and fails closed if a partially initialized server cannot prove the
    // selected leader's current state.
    SpawnSystem detached(nullptr, teamOneContext);
    const uint32_t detachedId = detached.AddSpawnLocation(valid);
    EXPECT_FALSE(detached.CanPlayerSpawnAt(101, detachedId));
}

RS2V_TEST_MAIN()
