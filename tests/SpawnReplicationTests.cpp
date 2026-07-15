#include "TestFramework.h"

#include "Network/SpawnReplication.h"

TEST(SpawnReplication, ReinforcementBaselineKeepsSpawnSceneAlive) {
    EXPECT_EQ(SpawnRepl::ResolveWireReinforcementCount(300u, 300u), 300);
    EXPECT_EQ(SpawnRepl::ResolveWireReinforcementCount(0u, 300u), 0);
    EXPECT_EQ(SpawnRepl::ResolveWireReinforcementCount(0u, 0u),
              SpawnRepl::kUnlimitedReinforcementsDisplay);
    EXPECT_EQ(SpawnRepl::ResolveWireReinforcementCount(UINT32_MAX, UINT32_MAX),
              INT32_MAX);

    BitWriter writer;
    SpawnRepl::WriteReinforcementsRemaining(writer, 300);
    EXPECT_EQ(writer.NumBits(), 38u); // captured h62 handle + signed int32

    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    EXPECT_EQ(reader.SerializeInt(SpawnRepl::kTeamInfoMaxHandle),
              SpawnRepl::kReinforcementsRemaining);
    EXPECT_EQ(reader.ReadInt32(), 300);
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), writer.NumBits());
}

TEST(SpawnReplication, RebaseStaticObjectRefTracksArtifactLayout) {
    EXPECT_EQ(SpawnRepl::RebaseStaticObjectRef(0u, 5u),
              std::optional<uint32_t>(0u));
    EXPECT_EQ(SpawnRepl::RebaseStaticObjectRef(289129u, 0u),
              std::optional<uint32_t>(289129u));
    EXPECT_EQ(SpawnRepl::RebaseStaticObjectRef(289129u, 5u),
              std::optional<uint32_t>(289134u));
    EXPECT_EQ(SpawnRepl::RebaseStaticObjectRef(289139u, 5u),
              std::optional<uint32_t>(289144u));
    EXPECT_EQ(SpawnRepl::RebaseStaticObjectRef(
                  ActorRepl::kStaticObjectMax - 1u, 0u),
              std::optional<uint32_t>(ActorRepl::kStaticObjectMax - 1u));
    EXPECT_FALSE(SpawnRepl::RebaseStaticObjectRef(
        ActorRepl::kStaticObjectMax - 1u, 1u).has_value());
    EXPECT_FALSE(SpawnRepl::RebaseStaticObjectRef(
        ActorRepl::kStaticObjectMax, 0u).has_value());
}

TEST(SpawnReplication, InstalledCompoundRefsRoundTripAfterRebase) {
    BitWriter writer;
    const auto us = SpawnRepl::RebaseStaticObjectRef(289129u, 5u);
    const auto nva = SpawnRepl::RebaseStaticObjectRef(289139u, 5u);
    ASSERT_TRUE(us.has_value());
    ASSERT_TRUE(nva.has_value());
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(writer, 0, *us));
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(writer, 1, *nva));

    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    for (const uint32_t expected : {289134u, 289144u}) {
        EXPECT_EQ(reader.SerializeInt(SpawnRepl::kTeamInfoMaxHandle), 59u);
        (void)reader.ReadByte();
        const ActorRepl::NetGUIDRef ref = ActorRepl::ReadNetGUID(reader);
        EXPECT_FALSE(ref.isDynamic);
        EXPECT_EQ(ref.index, expected);
    }
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), writer.NumBits());
}

TEST(SpawnReplication, ResortIslandVolumeUsesExactStaticObjectEncoding) {
    BitWriter writer;
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(
        writer, 0, 301195u)); // VNTE-Resort ROVolumePlayerStartGroup_4

    // h59 uses six ranged bits for this value, followed by the 8-bit static
    // array slot and a 32-bit static object ref (selector + 31-bit index).
    EXPECT_EQ(writer.NumBits(), 46u);

    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    EXPECT_EQ(reader.SerializeInt(SpawnRepl::kTeamInfoMaxHandle),
              SpawnRepl::kAvailableSpawnLocations);
    EXPECT_EQ(reader.ReadByte(), 0u);
    const ActorRepl::NetGUIDRef ref = ActorRepl::ReadNetGUID(reader);
    EXPECT_FALSE(ref.isDynamic);
    EXPECT_EQ(ref.index, 301195u);
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), writer.NumBits());
}

TEST(SpawnReplication, MultipleSlotsRemainCaptureAligned) {
    BitWriter writer;
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(writer, 0, 301188u));
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(writer, 1, 301189u));
    EXPECT_EQ(writer.NumBits(), 92u);

    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    for (uint8_t expectedSlot = 0; expectedSlot < 2; ++expectedSlot) {
        EXPECT_EQ(reader.SerializeInt(SpawnRepl::kTeamInfoMaxHandle), 59u);
        EXPECT_EQ(reader.ReadByte(), expectedSlot);
        const ActorRepl::NetGUIDRef ref = ActorRepl::ReadNetGUID(reader);
        EXPECT_FALSE(ref.isDynamic);
        EXPECT_EQ(ref.index, expectedSlot == 0 ? 301188u : 301189u);
    }
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), writer.NumBits());
}

TEST(SpawnReplication, EmptySlotUsesDynamicChannelZeroNone) {
    BitWriter writer;
    ASSERT_TRUE(SpawnRepl::WriteAvailableSpawnLocation(writer, 9, 0));
    EXPECT_EQ(writer.NumBits(), 25u);

    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    EXPECT_EQ(reader.SerializeInt(SpawnRepl::kTeamInfoMaxHandle), 59u);
    EXPECT_EQ(reader.ReadByte(), 9u);
    const ActorRepl::NetGUIDRef ref = ActorRepl::ReadNetGUID(reader);
    EXPECT_TRUE(ref.isDynamic);
    EXPECT_EQ(ref.index, 0u);
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), writer.NumBits());
}

TEST(SpawnReplication, InvalidInputDoesNotPartiallyWrite) {
    BitWriter writer;
    EXPECT_FALSE(SpawnRepl::WriteAvailableSpawnLocation(writer, 10, 301195u));
    EXPECT_FALSE(SpawnRepl::WriteAvailableSpawnLocation(
        writer, 0, ActorRepl::kStaticObjectMax));
    EXPECT_EQ(writer.NumBits(), 0u);
}

RS2V_TEST_MAIN()
