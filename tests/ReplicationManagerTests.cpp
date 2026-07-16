#include "TestFramework.h"

#include "Protocol/ActorReplication.h"
#include "Protocol/PropertyReplication.h"
#include "Protocol/ReplicationManager.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

PropertyState MakeProperty(uint32_t objectId) {
    PropertyState state{};
    state.objectId = objectId;
    state.flags = PR_HEALTH;
    state.health = 75;
    return state;
}

} // namespace

TEST(ReplicationManager, MissingSinkRetainsActorAndPropertyWork) {
    ReplicationManager manager;
    manager.RegisterActor(41);
    manager.MarkActorDirty(41, ActorRepFlag::HEALTH);
    manager.QueuePropertyUpdate(MakeProperty(41));

    manager.Tick(0.0f);

    std::vector<Packet> packets;
    manager.SetPacketSink([&](const Packet& packet) {
        packets.push_back(packet);
        return true;
    });
    manager.Tick(0.0f);

    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[0].GetTag(), std::string("ACTOR_REPLICATION"));
    EXPECT_EQ(packets[1].GetTag(), std::string("PROPERTY_REPLICATION"));
    EXPECT_EQ(ActorReplication::ParseReplicationPacket(packets[0]).size(), 1u);
    EXPECT_EQ(PropertyReplication::ParsePacket(packets[1]).size(), 1u);

    manager.Tick(0.0f);
    EXPECT_EQ(packets.size(), 2u);
}

TEST(ReplicationManager, RejectedSinkRetriesWithoutDuplicateAfterAcceptance) {
    int attempts = 0;
    std::vector<Packet> accepted;
    ReplicationManager manager([&](const Packet& packet) {
        ++attempts;
        if (attempts == 1) return false;
        accepted.push_back(packet);
        return true;
    });
    manager.RegisterActor(7);
    manager.MarkActorDirty(7, ActorRepFlag::POSITION);

    manager.Tick(0.0f);
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(accepted.empty());

    manager.Tick(0.0f);
    EXPECT_EQ(attempts, 2);
    ASSERT_EQ(accepted.size(), 1u);
    const auto actors = ActorReplication::ParseReplicationPacket(accepted[0]);
    ASSERT_EQ(actors.size(), 1u);
    EXPECT_EQ(actors[0].actorId, 7u);
    EXPECT_EQ(actors[0].stateFlags, static_cast<uint32_t>(ActorRepFlag::POSITION));

    manager.Tick(0.0f);
    EXPECT_EQ(attempts, 2);
}

TEST(ReplicationManager, ThrowingSinkRetainsPropertyQueue) {
    int calls = 0;
    ReplicationManager manager([&](const Packet&) -> bool {
        ++calls;
        throw std::runtime_error("transport unavailable");
    });
    manager.QueuePropertyUpdate(MakeProperty(99));

    EXPECT_NO_THROW(manager.Tick(0.0f));
    EXPECT_EQ(calls, 1);

    std::vector<Packet> packets;
    manager.SetPacketSink([&](const Packet& packet) {
        packets.push_back(packet);
        return true;
    });
    manager.Tick(0.0f);
    ASSERT_EQ(packets.size(), 1u);
    const auto properties = PropertyReplication::ParsePacket(packets[0]);
    ASSERT_EQ(properties.size(), 1u);
    EXPECT_EQ(properties[0].objectId, 99u);
}

TEST(ReplicationManager, SuccessfulSinkPreservesUpdatesQueuedDuringDispatch) {
    ReplicationManager* managerPtr = nullptr;
    std::vector<Packet> packets;
    bool appended = false;
    ReplicationManager manager([&](const Packet& packet) {
        packets.push_back(packet);
        if (!appended && packet.GetTag() == "PROPERTY_REPLICATION") {
            appended = true;
            managerPtr->QueuePropertyUpdate(MakeProperty(2));
        }
        return true;
    });
    managerPtr = &manager;
    manager.QueuePropertyUpdate(MakeProperty(1));

    manager.Tick(0.0f);
    ASSERT_EQ(packets.size(), 1u);
    auto first = PropertyReplication::ParsePacket(packets[0]);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].objectId, 1u);

    manager.Tick(0.0f);
    ASSERT_EQ(packets.size(), 2u);
    auto second = PropertyReplication::ParsePacket(packets[1]);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].objectId, 2u);
}

TEST(ReplicationManager, SuccessfulSinkPreservesSameActorBitMarkedDuringDispatch) {
    ReplicationManager* managerPtr = nullptr;
    std::vector<Packet> packets;
    bool markedAgain = false;
    ReplicationManager manager([&](const Packet& packet) {
        packets.push_back(packet);
        if (!markedAgain && packet.GetTag() == "ACTOR_REPLICATION") {
            markedAgain = true;
            managerPtr->MarkActorDirty(5, ActorRepFlag::HEALTH);
        }
        return true;
    });
    managerPtr = &manager;
    manager.RegisterActor(5);
    manager.MarkActorDirty(5, ActorRepFlag::HEALTH);

    manager.Tick(0.0f);
    ASSERT_EQ(packets.size(), 1u);
    manager.Tick(0.0f);
    ASSERT_EQ(packets.size(), 2u);
    manager.Tick(0.0f);
    EXPECT_EQ(packets.size(), 2u);
}

RS2V_TEST_MAIN()
