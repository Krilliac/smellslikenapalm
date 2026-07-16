#include "TestFramework.h"

#include "Game/PlayerManager.h"
#include "Game/SpawnSystem.h"
#include "Game/TeamMapping.h"
#include "Network/ClientConnection.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
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

struct PreparedSpawnFixture {
    static constexpr uint32_t kPlayerId = 101u;

    SpawnAccessContext access;
    PlayerManager players;
    SpawnSystem spawns;
    std::shared_ptr<ClientConnection> connection;
    std::shared_ptr<Player> player;
    std::function<void(int)> onAccessContextResolve;
    int accessContextResolves = 0;
    std::function<void(int)> onSquadEligibilityCheck;
    int squadEligibilityChecks = 0;

    PreparedSpawnFixture()
        : players(nullptr),
          spawns(
              nullptr,
              [this](uint32_t playerId)
                  -> std::optional<SpawnAccessContext> {
                  ++accessContextResolves;
                  if (onAccessContextResolve) {
                      onAccessContextResolve(accessContextResolves);
                  }
                  return playerId == kPlayerId
                      ? std::optional<SpawnAccessContext>{access}
                      : std::nullopt;
              },
              [this](uint32_t playerId, const SpawnLocation& location) {
                  ++squadEligibilityChecks;
                  if (onSquadEligibilityCheck) {
                      onSquadEligibilityCheck(squadEligibilityChecks);
                  }
                  return playerId == kPlayerId &&
                      location.squadLeaderId != 0;
              },
              &players) {
        access.playerTeam = TeamMapping::kServerUs;
        ConnectPlayer();
    }

    void ConnectPlayer() {
        connection = std::make_shared<ClientConnection>(
            kPlayerId, "127.0.0.1", uint16_t{7777}, nullptr, nullptr);
        connection->SetUE3Client(true);
        connection->SetPlayerName("PreparedSpawnTest");
        connection->SetTeamId(TeamMapping::kServerUs);
        players.OnPlayerConnect(connection);
        player = players.GetPlayer(kPlayerId);
    }

    uint32_t AddTunnel() {
        SpawnLocation tunnel =
            MakeSpawn(SpawnType::Tunnel, TeamMapping::kServerUs);
        tunnel.name = "prepared tunnel";
        tunnel.position = Vector3(120.0f, -45.0f, 8.0f);
        tunnel.rotation = Vector3(0.0f, 90.0f, 0.0f);
        tunnel.objectiveId = 7u;
        tunnel.tunnelHealth = 83;
        return spawns.AddSpawnLocation(tunnel);
    }

    uint32_t AddSquadLeader() {
        SpawnLocation leader =
            MakeSpawn(SpawnType::SquadLeader, TeamMapping::kServerUs);
        leader.name = "prepared squad leader";
        leader.position = Vector3(44.0f, 55.0f, 6.0f);
        leader.rotation = Vector3(0.0f, 180.0f, 0.0f);
        leader.squadLeaderId = 202u;
        return spawns.AddSpawnLocation(leader);
    }

    uint32_t AddBase() {
        SpawnLocation base =
            MakeSpawn(SpawnType::BaseSpawn, TeamMapping::kServerUs);
        base.name = "prepared base";
        base.position = Vector3(70.0f, 80.0f, 9.0f);
        base.rotation = Vector3(0.0f, 270.0f, 0.0f);
        return spawns.AddSpawnLocation(base);
    }
};

} // namespace

TEST(SpawnSystem, PreparePlayerSpawnIsNonMutatingAndCopyMoveSafe) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(4.0f, 5.0f, 6.0f));
    fixture.player->SetOrientation(Vector3(7.0f, 8.0f, 9.0f));
    fixture.player->SetHealth(37);
    const uint32_t spawnId = fixture.AddTunnel();
    const SpawnLocation before = *fixture.spawns.GetSpawnLocation(spawnId);

    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());

    EXPECT_EQ(fixture.player->GetPosition(), Vector3(4.0f, 5.0f, 6.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(7.0f, 8.0f, 9.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 37);
    const SpawnLocation* after = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->position, before.position);
    EXPECT_EQ(after->rotation, before.rotation);
    EXPECT_EQ(after->spawnCooldown, 0.0f);

    EXPECT_EQ(prepared->GetPlayerId(), PreparedSpawnFixture::kPlayerId);
    EXPECT_EQ(prepared->GetSpawnLocationId(), spawnId);
    EXPECT_EQ(prepared->GetSpawnType(), SpawnType::Tunnel);
    EXPECT_EQ(prepared->GetRotation(), before.rotation);
    EXPECT_EQ(prepared->GetPosition().z, before.position.z);
    EXPECT_TRUE(prepared->GetPosition().x >= before.position.x - 2.5f);
    EXPECT_TRUE(prepared->GetPosition().x <= before.position.x + 2.0f);
    EXPECT_TRUE(prepared->GetPosition().y >= before.position.y - 2.5f);
    EXPECT_TRUE(prepared->GetPosition().y <= before.position.y + 2.0f);

    SpawnSystem::PreparedPlayerSpawn copied = *prepared;
    SpawnSystem::PreparedPlayerSpawn moved = std::move(copied);
    EXPECT_EQ(moved.GetPlayerId(), prepared->GetPlayerId());
    EXPECT_EQ(moved.GetSpawnLocationId(), prepared->GetSpawnLocationId());
    EXPECT_EQ(moved.GetPosition(), prepared->GetPosition());
    EXPECT_EQ(moved.GetRotation(), prepared->GetRotation());
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnAppliesThePreparedTransform) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(-1.0f, -2.0f, -3.0f));
    const uint32_t spawnId = fixture.AddTunnel();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    const Vector3 preparedPosition = prepared->GetPosition();
    const Vector3 preparedRotation = prepared->GetRotation();

    EXPECT_TRUE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), preparedPosition);
    EXPECT_EQ(fixture.player->GetOrientation(), preparedRotation);
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Alive);
    EXPECT_EQ(fixture.player->GetHealth(), 100);
    const SpawnLocation* committed =
        fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->spawnCooldown, 10.0f);

    // The first commit changes both lifecycle and cooldown, so a copied token
    // cannot apply the authoritative transition again.
    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), preparedPosition);
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Alive);
    EXPECT_EQ(committed->spawnCooldown, 10.0f);
}

TEST(SpawnSystem, BaseSpawnPreparedTokenCannotReplayAnAlivePlayer) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    const uint32_t spawnId = fixture.AddBase();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    const Vector3 preparedPosition = prepared->GetPosition();

    ASSERT_TRUE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    ASSERT_EQ(fixture.player->GetState(), PlayerState::Alive);
    fixture.player->SetPosition(Vector3(1.0f, 2.0f, 3.0f));

    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_NE(fixture.player->GetPosition(), preparedPosition);
    fixture.players.OnPlayerDeath(PreparedSpawnFixture::kPlayerId);
    ASSERT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(1.0f, 2.0f, 3.0f));
    const SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, ExternalLifeTransitionInvalidatesPreparedToken) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    const uint32_t spawnId = fixture.AddBase();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    const uint64_t generationBefore =
        fixture.player->GetLifecycleGeneration();

    fixture.players.OnPlayerSpawn(PreparedSpawnFixture::kPlayerId);
    ASSERT_EQ(fixture.player->GetState(), PlayerState::Alive);
    ASSERT_NE(fixture.player->GetLifecycleGeneration(), generationBefore);
    fixture.players.OnPlayerDeath(PreparedSpawnFixture::kPlayerId);
    ASSERT_EQ(fixture.player->GetState(), PlayerState::Dead);
    fixture.player->SetPosition(Vector3(5.0f, 6.0f, 7.0f));

    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(5.0f, 6.0f, 7.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
}

TEST(SpawnSystem, PreparePlayerSpawnRejectsAnAlreadyAlivePlayer) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.players.OnPlayerSpawn(PreparedSpawnFixture::kPlayerId);
    ASSERT_EQ(fixture.player->GetState(), PlayerState::Alive);
    const uint32_t spawnId = fixture.AddBase();

    EXPECT_FALSE(fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId).has_value());
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Alive);
    const SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnRejectsAccessDriftWithoutMutation) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(10.0f, 20.0f, 30.0f));
    fixture.player->SetOrientation(Vector3(1.0f, 2.0f, 3.0f));
    fixture.player->SetHealth(41);
    const uint32_t spawnId = fixture.AddTunnel();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());

    fixture.access.playerTeam = TeamMapping::kServerNva;
    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(10.0f, 20.0f, 30.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 41);
    const SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnRejectsLocationDriftWithoutMutation) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(11.0f, 21.0f, 31.0f));
    fixture.player->SetOrientation(Vector3(4.0f, 5.0f, 6.0f));
    fixture.player->SetHealth(42);
    const uint32_t spawnId = fixture.AddTunnel();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());

    SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    location->position.x += 1.0f;
    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(11.0f, 21.0f, 31.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(4.0f, 5.0f, 6.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 42);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnRejectsReusedPlayerIdWithoutMutation) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    const uint32_t spawnId = fixture.AddTunnel();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    const std::shared_ptr<Player> originalIdentity = fixture.player;

    fixture.players.OnPlayerDisconnect(PreparedSpawnFixture::kPlayerId);
    fixture.ConnectPlayer();
    ASSERT_NE(fixture.player, nullptr);
    ASSERT_NE(fixture.player.get(), originalIdentity.get());
    fixture.player->SetPosition(Vector3(12.0f, 22.0f, 32.0f));
    fixture.player->SetOrientation(Vector3(7.0f, 8.0f, 9.0f));
    fixture.player->SetHealth(43);

    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(12.0f, 22.0f, 32.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(7.0f, 8.0f, 9.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 43);
    const SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnRejectsReentrantAccessDrift) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(13.0f, 23.0f, 33.0f));
    fixture.player->SetOrientation(Vector3(2.0f, 4.0f, 6.0f));
    fixture.player->SetHealth(44);
    fixture.onSquadEligibilityCheck = [&fixture](int check) {
        if (check == 2) {
            fixture.access.playerTeam = TeamMapping::kServerNva;
        }
    };
    const uint32_t spawnId = fixture.AddSquadLeader();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(fixture.squadEligibilityChecks, 1);

    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.squadEligibilityChecks, 2);
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(13.0f, 23.0f, 33.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(2.0f, 4.0f, 6.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 44);
    const SpawnLocation* location = fixture.spawns.GetSpawnLocation(spawnId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->spawnCooldown, 0.0f);
}

TEST(SpawnSystem, CommitPreparedPlayerSpawnRejectsResolverErasedLocation) {
    PreparedSpawnFixture fixture;
    ASSERT_NE(fixture.player, nullptr);
    fixture.player->SetPosition(Vector3(14.0f, 24.0f, 34.0f));
    fixture.player->SetOrientation(Vector3(3.0f, 6.0f, 9.0f));
    fixture.player->SetHealth(45);
    const uint32_t spawnId = fixture.AddTunnel();
    auto prepared = fixture.spawns.PreparePlayerSpawn(
        PreparedSpawnFixture::kPlayerId, spawnId);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(fixture.accessContextResolves, 1);
    fixture.onAccessContextResolve = [&fixture, spawnId](int check) {
        if (check == 2) fixture.spawns.RemoveSpawnLocation(spawnId);
    };

    EXPECT_FALSE(fixture.spawns.CommitPreparedPlayerSpawn(*prepared));
    EXPECT_EQ(fixture.accessContextResolves, 2);
    EXPECT_EQ(fixture.player->GetPosition(), Vector3(14.0f, 24.0f, 34.0f));
    EXPECT_EQ(fixture.player->GetOrientation(), Vector3(3.0f, 6.0f, 9.0f));
    EXPECT_EQ(fixture.player->GetState(), PlayerState::Dead);
    EXPECT_EQ(fixture.player->GetHealth(), 45);
    EXPECT_EQ(fixture.spawns.GetSpawnLocation(spawnId), nullptr);
}

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
