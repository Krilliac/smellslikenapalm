#include "Game/RoleSystem.h"
#include "TestFramework.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

class RoleSystemLifecycleTestHarness {
public:
    static void SeedCommander(RoleSystem& roles, uint32_t teamId,
                              uint32_t playerId) {
        roles.m_teamCommanders[teamId] = playerId;
        roles.m_playerRoles[playerId] = CombatRole::Commander;
    }

    static bool HasRoleEntry(const RoleSystem& roles, uint32_t playerId) {
        return roles.m_playerRoles.find(playerId) != roles.m_playerRoles.end();
    }

    static size_t CommanderEntryCount(const RoleSystem& roles) {
        return roles.m_teamCommanders.size();
    }

    static size_t SquadCount(const RoleSystem& roles) {
        return roles.m_squads.size();
    }

    static void SeedRole(RoleSystem& roles, uint32_t playerId,
                         CombatRole role) {
        roles.m_playerRoles[playerId] = role;
    }

    static void AddStaleMembership(RoleSystem& roles, uint32_t squadId,
                                   uint32_t playerId, bool leader) {
        auto squad = roles.m_squads.find(squadId);
        if (squad == roles.m_squads.end()) return;
        squad->second.memberIds.push_back(playerId);
        if (leader) squad->second.leaderId = playerId;
    }

    static void SeedRetailOwner(RoleSystem& roles, uint32_t teamId,
                                uint8_t squadIndex, uint8_t roleIndex,
                                uint32_t playerId) {
        roles.m_retailSquads[teamId - 1][squadIndex]
            .slotOwnerIds[roleIndex] = playerId;
    }

    static void SeedRetailAssignment(RoleSystem& roles, uint32_t playerId,
                                     uint32_t teamId, uint8_t squadIndex,
                                     uint8_t roleIndex,
                                     uint32_t generation) {
        roles.m_retailSquadAssignments[playerId] = RetailSquadAssignment{
            teamId, squadIndex, roleIndex, generation};
    }

    static bool HasRawRetailAssignment(const RoleSystem& roles,
                                       uint32_t playerId) {
        return roles.m_retailSquadAssignments.find(playerId) !=
               roles.m_retailSquadAssignments.end();
    }

    static void SeedRetailLock(RoleSystem& roles, uint32_t teamId,
                               uint8_t squadIndex) {
        roles.m_retailSquads[teamId - 1][squadIndex].locked = true;
    }
};

TEST(RoleSystemLifecycle, CommanderOwnershipIsFullyReleasedAndIdempotent) {
    RoleSystem roles(nullptr);

    // Duplicate ownership cannot arise through the public API, but cleanup is
    // deliberately a recovery boundary and must not leave a stale team slot.
    RoleSystemLifecycleTestHarness::SeedCommander(roles, 1, 42);
    RoleSystemLifecycleTestHarness::SeedCommander(roles, 2, 42);
    ASSERT_EQ(RoleSystemLifecycleTestHarness::CommanderEntryCount(roles), 2u);

    roles.RemovePlayer(42);
    EXPECT_FALSE(roles.HasCommander(1));
    EXPECT_FALSE(roles.HasCommander(2));
    EXPECT_EQ(RoleSystemLifecycleTestHarness::CommanderEntryCount(roles), 0u);
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, 42));

    roles.RemovePlayer(42);
    EXPECT_EQ(RoleSystemLifecycleTestHarness::CommanderEntryCount(roles), 0u);
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, 42));

    // Reusing the numeric ID or the released team slot starts from clean state.
    RoleSystemLifecycleTestHarness::SeedCommander(roles, 1, 42);
    EXPECT_TRUE(roles.HasCommander(1));
    EXPECT_EQ(roles.GetTeamCommander(1), 42u);
    EXPECT_EQ(roles.GetPlayerRole(42), CombatRole::Commander);
}

TEST(RoleSystemLifecycle, FullSquadRemovalPromotesDeterministicallyAndAllowsReuse) {
    RoleSystem roles(nullptr);
    const uint32_t reservedSquadId = roles.CreateSquad(1, "Reserved");
    const uint32_t squadId = roles.CreateSquad(1, "Lifecycle");

    for (uint32_t playerId = 1; playerId <= Squad::MAX_SQUAD_SIZE; ++playerId) {
        ASSERT_TRUE(roles.JoinSquad(playerId, squadId));
    }
    ASSERT_TRUE(roles.GetSquad(squadId)->IsFull());
    ASSERT_EQ(roles.GetSquad(squadId)->leaderId, 1u);

    // A non-leader departure preserves the existing leader.
    roles.RemovePlayer(4);
    ASSERT_NE(roles.GetSquad(squadId), nullptr);
    EXPECT_EQ(roles.GetSquad(squadId)->leaderId, 1u);
    EXPECT_EQ(roles.GetPlayerSquad(4), 0u);
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, 4));

    // Leader promotion follows stable join order.
    roles.RemovePlayer(1);
    ASSERT_NE(roles.GetSquad(squadId), nullptr);
    EXPECT_EQ(roles.GetSquad(squadId)->leaderId, 2u);
    EXPECT_EQ(roles.GetPlayerRole(2), CombatRole::SquadLeader);

    for (uint32_t playerId : {2u, 3u, 5u, 6u}) {
        roles.RemovePlayer(playerId);
    }
    EXPECT_EQ(roles.GetSquad(squadId), nullptr);
    // Initialize deliberately pre-creates empty squads. Cleanup deletes only
    // the affected squad, not unrelated empty slots awaiting later members.
    EXPECT_NE(roles.GetSquad(reservedSquadId), nullptr);
    EXPECT_EQ(RoleSystemLifecycleTestHarness::SquadCount(roles), 1u);

    for (uint32_t playerId = 1; playerId <= Squad::MAX_SQUAD_SIZE; ++playerId) {
        EXPECT_EQ(roles.GetPlayerSquad(playerId), 0u);
        EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, playerId));
        roles.RemovePlayer(playerId);
    }

    ASSERT_TRUE(roles.JoinSquad(1, reservedSquadId));
    EXPECT_EQ(roles.GetPlayerSquad(1), reservedSquadId);
    EXPECT_EQ(roles.GetSquad(reservedSquadId)->leaderId, 1u);
    EXPECT_EQ(roles.GetPlayerRole(1), CombatRole::SquadLeader);
}

TEST(RoleSystemLifecycle, CleanupSweepsStaleDuplicateSquadReferences) {
    RoleSystem roles(nullptr);
    const uint32_t alpha = roles.CreateSquad(1, "Alpha");
    const uint32_t bravo = roles.CreateSquad(1, "Bravo");
    ASSERT_TRUE(roles.JoinSquad(10, alpha));
    ASSERT_TRUE(roles.JoinSquad(11, alpha));
    ASSERT_TRUE(roles.JoinSquad(12, bravo));

    // Simulate modest state drift: player 10 is still reverse-mapped to Alpha
    // but is also present as Bravo's stale leader.
    RoleSystemLifecycleTestHarness::AddStaleMembership(
        roles, bravo, 10, true);

    roles.RemovePlayer(10);
    ASSERT_NE(roles.GetSquad(alpha), nullptr);
    ASSERT_NE(roles.GetSquad(bravo), nullptr);
    EXPECT_EQ(roles.GetSquad(alpha)->leaderId, 11u);
    EXPECT_EQ(roles.GetSquad(bravo)->leaderId, 12u);
    EXPECT_EQ(roles.GetPlayerRole(11), CombatRole::SquadLeader);
    EXPECT_EQ(roles.GetPlayerRole(12), CombatRole::SquadLeader);
    EXPECT_EQ(roles.GetPlayerSquad(10), 0u);
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, 10));
}

TEST(RoleSystemLifecycle, RetailSquadCountMatchesSourceThresholds) {
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 1), 2u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 12), 2u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 13), 4u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Supremacy", 24), 4u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 25), 8u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 32), 8u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", 33), 10u);

    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Skirmish", 1), 1u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("SKIRMISH", 12), 1u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("ROGameInfoSkirmish", 13),
              2u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("skirmish", 24), 2u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Skirmish", 25), 8u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Skirmish", 32), 8u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Skirmish", 33), 10u);

    // Missing or invalid capacity safely uses the ordinary 64-player branch.
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Skirmish", 0), 10u);
    EXPECT_EQ(RoleSystem::ResolveRetailSquadCount("Territories", -1), 10u);
}

TEST(RoleSystemLifecycle, RetailConfigurationLimitsActiveStablePrefix) {
    RoleSystem roles(nullptr);
    const RetailSquadAssignment before = roles.AutoAssignRetailSquad(900, 1);
    ASSERT_TRUE(before.IsValid());
    ASSERT_TRUE(roles.SetRetailSquadLocked(1, 0, true));

    const uint32_t generation =
        roles.ConfigureRetailSquads("Territories", 12);
    EXPECT_EQ(roles.GetActiveRetailSquadCount(), 2u);
    EXPECT_NE(generation, before.generation);
    EXPECT_FALSE(roles.GetRetailSquadAssignment(900).has_value());
    EXPECT_FALSE(roles.IsRetailSquadLocked(1, 0));

    for (uint32_t playerId = 1; playerId <= 12; ++playerId) {
        ASSERT_TRUE(roles.AutoAssignRetailSquad(playerId, 1).IsValid());
    }
    EXPECT_TRUE(roles.GetRetailSquad(1, 0)->IsFull());
    EXPECT_TRUE(roles.GetRetailSquad(1, 1)->IsFull());
    EXPECT_FALSE(roles.AutoAssignRetailSquad(13, 1).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(13, 1, 2).IsValid());

    // The ten stable replication entries remain addressable but inactive
    // indices cannot be locked or joined.
    ASSERT_NE(roles.GetRetailSquad(1, 2), nullptr);
    EXPECT_EQ(roles.GetRetailSquad(1, 2)->Occupancy(), 0u);
    EXPECT_FALSE(roles.SetRetailSquadLocked(1, 2, true));
}

TEST(RoleSystemLifecycle, RetailLocksSkipNewJoinsAndPreserveAssignments) {
    RoleSystem roles(nullptr);
    const uint32_t generation =
        roles.ConfigureRetailSquads("Territories", 12);
    ASSERT_EQ(generation, roles.GetRetailSquadGeneration());

    ASSERT_TRUE(roles.JoinRetailSquad(1, 1, 0).IsValid());
    ASSERT_TRUE(roles.JoinRetailSquad(2, 1, 0).IsValid());
    ASSERT_TRUE(roles.JoinRetailSquad(3, 1, 0).IsValid());
    ASSERT_TRUE(roles.JoinRetailSquad(10, 1, 1).IsValid());
    ASSERT_TRUE(roles.SetRetailSquadLocked(1, 0, true));
    EXPECT_TRUE(roles.IsRetailSquadLocked(1, 0));

    // The fullest squad is locked, so automatic assignment uses squad one.
    const RetailSquadAssignment automatic =
        roles.AutoAssignRetailSquad(11, 1);
    ASSERT_TRUE(automatic.IsValid());
    EXPECT_EQ(automatic.squadIndex, 1u);
    EXPECT_EQ(automatic.roleIndex, 1u);

    // Existing ownership remains idempotent after locking. A move into the
    // locked squad is rejected transactionally and preserves the source slot.
    const RetailSquadAssignment repeated = roles.JoinRetailSquad(2, 1, 0);
    ASSERT_TRUE(repeated.IsValid());
    EXPECT_EQ(repeated.squadIndex, 0u);
    EXPECT_EQ(repeated.roleIndex, 1u);
    EXPECT_FALSE(roles.JoinRetailSquad(11, 1, 0).IsValid());
    const auto retained = roles.GetRetailSquadAssignment(11);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->squadIndex, 1u);
    EXPECT_EQ(retained->roleIndex, 1u);

    ASSERT_TRUE(roles.SetRetailSquadLocked(1, 1, true));
    EXPECT_FALSE(roles.AutoAssignRetailSquad(12, 1).IsValid());
    EXPECT_TRUE(roles.AutoAssignRetailSquad(11, 1).IsValid());

    roles.ResetRetailSquads();
    EXPECT_EQ(roles.GetActiveRetailSquadCount(), 2u);
    EXPECT_FALSE(roles.IsRetailSquadLocked(1, 0));
    EXPECT_FALSE(roles.IsRetailSquadLocked(1, 1));
}

TEST(RoleSystemLifecycle, RetailReconciliationPurgesInactiveRawState) {
    RoleSystem roles(nullptr);
    roles.ConfigureRetailSquads("Territories", 12);
    const uint32_t generation = roles.GetRetailSquadGeneration();
    RoleSystemLifecycleTestHarness::SeedRetailOwner(roles, 1, 8, 3, 700);
    RoleSystemLifecycleTestHarness::SeedRetailAssignment(
        roles, 700, 1, 8, 3, generation);
    RoleSystemLifecycleTestHarness::SeedRetailLock(roles, 1, 8);

    ASSERT_TRUE(roles.AutoAssignRetailSquad(1, 1).IsValid());
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRawRetailAssignment(
        roles, 700));
    EXPECT_EQ(roles.GetRetailSquad(1, 8)->slotOwnerIds[3], 0u);
    EXPECT_EQ(roles.GetRetailSquad(1, 8)->leaderId, 0u);
    EXPECT_FALSE(roles.GetRetailSquad(1, 8)->locked);
}

TEST(RoleSystemLifecycle, RetailAllocatorPacksFullestSquadWithStableIndices) {
    RoleSystem roles(nullptr);

    for (uint32_t playerId = 1;
         playerId <= RoleSystem::RETAIL_SQUAD_COUNT * RetailSquad::SLOT_COUNT;
         ++playerId) {
        const RetailSquadAssignment assignment =
            roles.AutoAssignRetailSquad(playerId, 1);
        ASSERT_TRUE(assignment.IsValid());
        EXPECT_EQ(assignment.teamId, 1u);
        EXPECT_EQ(assignment.squadIndex,
                  static_cast<uint8_t>((playerId - 1) /
                                       RetailSquad::SLOT_COUNT));
        EXPECT_EQ(assignment.roleIndex,
                  static_cast<uint8_t>((playerId - 1) %
                                       RetailSquad::SLOT_COUNT));
        EXPECT_EQ(assignment.generation,
                  roles.GetRetailSquadGeneration());
    }

    // All ten stable squad indices exist for each retail team.
    for (uint32_t teamId = 1; teamId <= RoleSystem::RETAIL_TEAM_COUNT;
         ++teamId) {
        for (uint8_t squadIndex = 0;
             squadIndex < RoleSystem::RETAIL_SQUAD_COUNT; ++squadIndex) {
            ASSERT_NE(roles.GetRetailSquad(teamId, squadIndex), nullptr);
        }
    }
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->Occupancy(),
              static_cast<size_t>(RetailSquad::SLOT_COUNT));
    EXPECT_EQ(roles.GetRetailSquad(1, 9)->Occupancy(),
              static_cast<size_t>(RetailSquad::SLOT_COUNT));
    EXPECT_EQ(roles.GetRetailSquad(1, 10), nullptr);

    const RetailSquadAssignment overflow =
        roles.AutoAssignRetailSquad(61, 1);
    EXPECT_FALSE(overflow.IsValid());

    // Re-selecting an existing assignment is idempotent even at capacity.
    const RetailSquadAssignment repeated =
        roles.AutoAssignRetailSquad(7, 1);
    EXPECT_TRUE(repeated.IsValid());
    EXPECT_EQ(repeated.squadIndex, 1u);
    EXPECT_EQ(repeated.roleIndex, 0u);
}

TEST(RoleSystemLifecycle, RetailReleaseReusesExactHoleAndKeepsRoleSeparate) {
    RoleSystem roles(nullptr);
    RoleSystemLifecycleTestHarness::SeedRole(
        roles, 1, CombatRole::Pointman);
    RoleSystemLifecycleTestHarness::SeedRole(
        roles, 2, CombatRole::CombatEngineer);

    for (uint32_t playerId = 1; playerId <= 8; ++playerId) {
        ASSERT_TRUE(roles.AutoAssignRetailSquad(playerId, 1).IsValid());
    }
    EXPECT_TRUE(roles.IsRetailSquadLeader(1));
    EXPECT_EQ(roles.GetPlayerRole(1), CombatRole::Pointman);

    ASSERT_TRUE(roles.ReleaseRetailSquadAssignment(3));
    EXPECT_FALSE(roles.GetRetailSquadAssignment(3).has_value());
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->slotOwnerIds[2], 0u);

    // Squad zero has five owners versus squad one's two, so it is still the
    // fullest eligible squad. Its first retained hole is role index two.
    const RetailSquadAssignment replacement =
        roles.AutoAssignRetailSquad(9, 1);
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.squadIndex, 0u);
    EXPECT_EQ(replacement.roleIndex, 2u);

    ASSERT_TRUE(roles.ReleaseRetailSquadAssignment(1));
    EXPECT_FALSE(roles.ReleaseRetailSquadAssignment(1));
    EXPECT_EQ(roles.GetRetailSquadLeader(1, 0), 2u);
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->slotOwnerIds[0], 2u);
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->slotOwnerIds[1], 0u);
    ASSERT_TRUE(roles.GetRetailSquadAssignment(2).has_value());
    EXPECT_EQ(roles.GetRetailSquadAssignment(2)->roleIndex, 0u);
    EXPECT_EQ(roles.GetPlayerRole(1), CombatRole::Pointman);
    EXPECT_EQ(roles.GetPlayerRole(2), CombatRole::CombatEngineer);
    EXPECT_TRUE(roles.IsRetailSquadLeader(2));

    // Changing teams releases exactly the former team slot, then allocates
    // independently from the target team's stable ten-squad roster.
    const RetailSquadAssignment moved =
        roles.AutoAssignRetailSquad(2, 2);
    ASSERT_TRUE(moved.IsValid());
    EXPECT_EQ(moved.teamId, 2u);
    EXPECT_EQ(moved.squadIndex, 0u);
    EXPECT_EQ(moved.roleIndex, 0u);
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->slotOwnerIds[1], 0u);
    EXPECT_EQ(roles.GetPlayerRole(2), CombatRole::CombatEngineer);
    ASSERT_TRUE(roles.GetRetailSquadAssignment(9).has_value());
    EXPECT_EQ(roles.GetRetailSquadAssignment(9)->roleIndex, 0u);

    roles.RemovePlayer(2);
    EXPECT_FALSE(roles.GetRetailSquadAssignment(2).has_value());
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRoleEntry(roles, 2));
}

TEST(RoleSystemLifecycle, RetailCrossTeamMoveKeepsSourceWhenTargetIsFull) {
    RoleSystem roles(nullptr);
    const RetailSquadAssignment source =
        roles.AutoAssignRetailSquad(42, 1);
    ASSERT_TRUE(source.IsValid());

    for (uint32_t playerId = 1000;
         playerId < 1000 +
             RoleSystem::RETAIL_SQUAD_COUNT * RetailSquad::SLOT_COUNT;
         ++playerId) {
        ASSERT_TRUE(roles.AutoAssignRetailSquad(playerId, 2).IsValid());
    }

    const RetailSquadAssignment rejected =
        roles.AutoAssignRetailSquad(42, 2);
    EXPECT_FALSE(rejected.IsValid());
    const auto retained = roles.GetRetailSquadAssignment(42);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->teamId, source.teamId);
    EXPECT_EQ(retained->squadIndex, source.squadIndex);
    EXPECT_EQ(retained->roleIndex, source.roleIndex);
    EXPECT_EQ(roles.GetRetailSquad(1, source.squadIndex)
                  ->slotOwnerIds[source.roleIndex],
              42u);
    EXPECT_TRUE(roles.IsRetailSquadLeader(42));
}

TEST(RoleSystemLifecycle, RetailExplicitJoinUsesFirstHoleAndIsIdempotent) {
    RoleSystem roles(nullptr);

    ASSERT_EQ(roles.JoinRetailSquad(10, 1, 3).roleIndex, 0u);
    ASSERT_EQ(roles.JoinRetailSquad(20, 1, 3).roleIndex, 1u);
    ASSERT_EQ(roles.JoinRetailSquad(30, 1, 3).roleIndex, 2u);
    ASSERT_TRUE(roles.LeaveRetailSquad(20));

    const RetailSquadAssignment joined = roles.JoinRetailSquad(40, 1, 3);
    ASSERT_TRUE(joined.IsValid());
    EXPECT_EQ(joined.teamId, 1u);
    EXPECT_EQ(joined.squadIndex, 3u);
    EXPECT_EQ(joined.roleIndex, 1u);

    const RetailSquadAssignment repeated = roles.JoinRetailSquad(40, 1, 3);
    EXPECT_TRUE(repeated.IsValid());
    EXPECT_EQ(repeated.teamId, joined.teamId);
    EXPECT_EQ(repeated.squadIndex, joined.squadIndex);
    EXPECT_EQ(repeated.roleIndex, joined.roleIndex);
    EXPECT_EQ(roles.GetRetailSquad(1, 3)->Occupancy(), 3u);
}

TEST(RoleSystemLifecycle, RetailExplicitJoinRejectsInvalidAndFullTargets) {
    RoleSystem roles(nullptr);
    const RetailSquadAssignment source = roles.JoinRetailSquad(42, 1, 2);
    ASSERT_TRUE(source.IsValid());

    for (uint32_t playerId = 100; playerId < 106; ++playerId) {
        ASSERT_TRUE(roles.JoinRetailSquad(playerId, 2, 8).IsValid());
    }
    ASSERT_TRUE(roles.GetRetailSquad(2, 8)->IsFull());

    // Rejoining one's current target remains valid even when every slot is
    // occupied; the player already owns capacity and must not be displaced.
    const RetailSquadAssignment repeated = roles.JoinRetailSquad(100, 2, 8);
    EXPECT_TRUE(repeated.IsValid());
    EXPECT_EQ(repeated.squadIndex, 8u);
    EXPECT_EQ(repeated.roleIndex, 0u);

    EXPECT_FALSE(roles.JoinRetailSquad(42, 2, 8).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(42, 0, 0).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(42, 3, 0).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(42, 1, 10).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(0, 1, 0).IsValid());

    const auto retained = roles.GetRetailSquadAssignment(42);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->teamId, source.teamId);
    EXPECT_EQ(retained->squadIndex, source.squadIndex);
    EXPECT_EQ(retained->roleIndex, source.roleIndex);
    EXPECT_EQ(roles.GetRetailSquad(1, source.squadIndex)
                  ->slotOwnerIds[source.roleIndex],
              42u);
}

TEST(RoleSystemLifecycle, RetailExplicitMovePromotesSourceLeaderAndKeepsRole) {
    RoleSystem roles(nullptr);
    RoleSystemLifecycleTestHarness::SeedRole(
        roles, 10, CombatRole::CombatEngineer);
    RoleSystemLifecycleTestHarness::SeedRole(
        roles, 20, CombatRole::Pointman);

    ASSERT_EQ(roles.JoinRetailSquad(10, 1, 4).roleIndex, 0u);
    ASSERT_EQ(roles.JoinRetailSquad(20, 1, 4).roleIndex, 1u);
    ASSERT_EQ(roles.JoinRetailSquad(30, 1, 4).roleIndex, 2u);

    const RetailSquadAssignment moved = roles.JoinRetailSquad(10, 1, 7);
    ASSERT_TRUE(moved.IsValid());
    EXPECT_EQ(moved.squadIndex, 7u);
    EXPECT_EQ(moved.roleIndex, 0u);

    const RetailSquad* source = roles.GetRetailSquad(1, 4);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->leaderId, 20u);
    EXPECT_EQ(source->slotOwnerIds[0], 20u);
    EXPECT_EQ(source->slotOwnerIds[1], 0u);
    EXPECT_EQ(source->slotOwnerIds[2], 30u);
    ASSERT_TRUE(roles.GetRetailSquadAssignment(20).has_value());
    EXPECT_EQ(roles.GetRetailSquadAssignment(20)->roleIndex, 0u);

    EXPECT_EQ(roles.GetPlayerRole(10), CombatRole::CombatEngineer);
    EXPECT_EQ(roles.GetPlayerRole(20), CombatRole::Pointman);
}

TEST(RoleSystemLifecycle, RetailExplicitLeaveIsIdempotentAndKeepsRole) {
    RoleSystem roles(nullptr);
    RoleSystemLifecycleTestHarness::SeedRole(
        roles, 77, CombatRole::Grenadier);
    ASSERT_TRUE(roles.JoinRetailSquad(77, 2, 5).IsValid());

    EXPECT_TRUE(roles.LeaveRetailSquad(77));
    EXPECT_FALSE(roles.LeaveRetailSquad(77));
    EXPECT_FALSE(roles.GetRetailSquadAssignment(77).has_value());
    EXPECT_EQ(roles.GetRetailSquad(2, 5)->Occupancy(), 0u);
    EXPECT_EQ(roles.GetRetailSquadLeader(2, 5), 0u);
    EXPECT_EQ(roles.GetPlayerRole(77), CombatRole::Grenadier);
}

TEST(RoleSystemLifecycle, RetailLeaderPromotionUsesLowestOccupiedRoleSlot) {
    RoleSystem roles(nullptr);
    ASSERT_EQ(roles.AutoAssignRetailSquad(50, 1).roleIndex, 0u);
    ASSERT_EQ(roles.AutoAssignRetailSquad(900, 1).roleIndex, 1u);
    ASSERT_EQ(roles.AutoAssignRetailSquad(10, 1).roleIndex, 2u);

    ASSERT_TRUE(roles.ReleaseRetailSquadAssignment(50));
    const RetailSquad* squad = roles.GetRetailSquad(1, 0);
    ASSERT_NE(squad, nullptr);
    EXPECT_EQ(squad->slotOwnerIds[0], 900u);
    EXPECT_EQ(squad->slotOwnerIds[1], 0u);
    EXPECT_EQ(squad->slotOwnerIds[2], 10u);
    EXPECT_EQ(squad->leaderId, 900u);

    const auto promoted = roles.GetRetailSquadAssignment(900);
    ASSERT_TRUE(promoted.has_value());
    EXPECT_EQ(promoted->roleIndex, 0u);
    const auto retained = roles.GetRetailSquadAssignment(10);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->roleIndex, 2u);
}

TEST(RoleSystemLifecycle, RetailReleaseReconcilesOrphansAndDuplicates) {
    RoleSystem roles(nullptr);
    ASSERT_TRUE(roles.AutoAssignRetailSquad(1, 1).IsValid());
    ASSERT_TRUE(roles.AutoAssignRetailSquad(2, 1).IsValid());
    ASSERT_TRUE(roles.AutoAssignRetailSquad(3, 1).IsValid());

    const uint32_t generation = roles.GetRetailSquadGeneration();
    // A duplicate owner slot should preserve the location named by the valid
    // reverse record and clear the untracked duplicate.
    RoleSystemLifecycleTestHarness::SeedRetailOwner(
        roles, 2, 9, 5, 2);
    // An owner with no reverse record is an orphan.
    RoleSystemLifecycleTestHarness::SeedRetailOwner(
        roles, 2, 8, 4, 777);
    // Invalid-generation reverse state and its slot must both be removed.
    RoleSystemLifecycleTestHarness::SeedRetailOwner(
        roles, 2, 7, 3, 888);
    RoleSystemLifecycleTestHarness::SeedRetailAssignment(
        roles, 888, 2, 7, 3, generation + 1);
    // A unique slot repairs stale coordinates in an otherwise live reverse
    // record; a reverse-only entry is removed.
    RoleSystemLifecycleTestHarness::SeedRetailAssignment(
        roles, 3, 1, 0, 5, generation);
    RoleSystemLifecycleTestHarness::SeedRetailAssignment(
        roles, 999, 2, 6, 2, generation);

    ASSERT_TRUE(roles.ReleaseRetailSquadAssignment(1));

    const RetailSquad* squad = roles.GetRetailSquad(1, 0);
    ASSERT_NE(squad, nullptr);
    EXPECT_EQ(squad->slotOwnerIds[0], 2u);
    EXPECT_EQ(squad->slotOwnerIds[1], 0u);
    EXPECT_EQ(squad->slotOwnerIds[2], 3u);
    EXPECT_EQ(squad->leaderId, 2u);

    const auto promoted = roles.GetRetailSquadAssignment(2);
    ASSERT_TRUE(promoted.has_value());
    EXPECT_EQ(promoted->teamId, 1u);
    EXPECT_EQ(promoted->squadIndex, 0u);
    EXPECT_EQ(promoted->roleIndex, 0u);
    const auto repaired = roles.GetRetailSquadAssignment(3);
    ASSERT_TRUE(repaired.has_value());
    EXPECT_EQ(repaired->roleIndex, 2u);

    EXPECT_EQ(roles.GetRetailSquad(2, 9)->slotOwnerIds[5], 0u);
    EXPECT_EQ(roles.GetRetailSquad(2, 8)->slotOwnerIds[4], 0u);
    EXPECT_EQ(roles.GetRetailSquad(2, 7)->slotOwnerIds[3], 0u);
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRawRetailAssignment(
        roles, 777));
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRawRetailAssignment(
        roles, 888));
    EXPECT_FALSE(RoleSystemLifecycleTestHarness::HasRawRetailAssignment(
        roles, 999));
}

TEST(RoleSystemLifecycle, RetailResetStartsFreshNonZeroGeneration) {
    RoleSystem roles(nullptr);
    const RetailSquadAssignment before =
        roles.AutoAssignRetailSquad(100, 1);
    ASSERT_TRUE(before.IsValid());

    const uint32_t nextGeneration = roles.ResetRetailSquads();
    EXPECT_NE(nextGeneration, 0u);
    EXPECT_NE(nextGeneration, before.generation);
    EXPECT_EQ(nextGeneration, roles.GetRetailSquadGeneration());
    EXPECT_FALSE(roles.GetRetailSquadAssignment(100).has_value());
    EXPECT_EQ(roles.GetRetailSquad(1, 0)->Occupancy(), 0u);

    const RetailSquadAssignment after =
        roles.AutoAssignRetailSquad(100, 1);
    ASSERT_TRUE(after.IsValid());
    EXPECT_EQ(after.generation, nextGeneration);
    EXPECT_EQ(after.squadIndex, 0u);
    EXPECT_EQ(after.roleIndex, 0u);

    EXPECT_FALSE(roles.AutoAssignRetailSquad(0, 1).IsValid());
    EXPECT_FALSE(roles.AutoAssignRetailSquad(101, 0).IsValid());
    EXPECT_FALSE(roles.AutoAssignRetailSquad(101, 3).IsValid());
}

TEST(RoleSystemLifecycle, CompoundMappedFactionsDefineEveryMappedRole) {
    RoleSystem roles(nullptr);
    roles.Initialize();

    roles.SetTeamFaction(1, Faction::USMC);
    const RoleLoadout engineer =
        roles.GetRoleLoadout(CombatRole::CombatEngineer, Faction::USMC);
    EXPECT_GT(roles.GetRoleLimit(1, CombatRole::CombatEngineer), 0);
    EXPECT_FALSE(engineer.primaryWeapon.empty());

    roles.SetTeamFaction(2, Faction::NLFSV);
    const RoleLoadout scout =
        roles.GetRoleLoadout(CombatRole::Pointman, Faction::NLFSV);
    EXPECT_GT(roles.GetRoleLimit(2, CombatRole::Pointman), 0);
    EXPECT_FALSE(scout.primaryWeapon.empty());
}

RS2V_TEST_MAIN()
