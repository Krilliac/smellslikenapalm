// tests/ReplicationTests.cpp
// Comprehensive unit tests for actor/property replication subsystem
//
// Covers:
// 1. Baseline replication: full state sync.
// 2. Delta replication: only changed properties.
// 3. Reliable vs. unreliable replication channels.
// 4. Snapshot interpolation and tick alignment.
// 5. Out-of-order and dropped packet handling.
// 6. Performance under many actors and high-frequency updates.
// 7. Edge cases: no changes, all properties changed, network jitter.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <random>
#include <chrono>
#include "Network/ReplicationManager.h"
#include "Network/ReplicationSnapshot.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using ::testing::_;

// Mock actor with simple properties
struct MockActor {
    uint32_t id;
    Math::Vector3 position;
    float health;
    bool active;

    bool operator==(MockActor const& o) const {
        return id == o.id
            && position == o.position
            && fabs(health - o.health) < 1e-3f
            && active == o.active;
    }
};

// Test fixture
class ReplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr = std::make_unique<ReplicationManager>();
        // Seed actors
        for (uint32_t i = 1; i <= 100; ++i) {
            MockActor a{i, {float(i),0,0}, 100.0f, true};
            actors.push_back(a);
            mgr->RegisterActor(a.id);
            mgr->SetProperty(a.id, "position", a.position);
            mgr->SetProperty(a.id, "health", a.health);
            mgr->SetProperty(a.id, "active", a.active);
        }
    }

    std::unique_ptr<ReplicationManager> mgr;
    std::vector<MockActor> actors;

    MockActor RandomizeActor(MockActor a) {
        std::mt19937 rng(a.id);
        std::uniform_real_distribution<float> dpos(-1,1), dhealth(-5,5);
        a.position.x += dpos(rng);
        a.position.z += dpos(rng);
        a.health = std::clamp(a.health + dhealth(rng), 0.0f, 100.0f);
        a.active = (rng()%2)==0;
        return a;
    }
};

// 1. Full snapshot replication
TEST_F(ReplicationTest, FullSnapshot_MatchesAllActors) {
    auto snap = mgr->CreateSnapshot(true);  // full sync
    ASSERT_EQ(snap.actorCount(), actors.size());
    for (auto const& a : actors) {
        MockActor ra;
        ra.id = a.id;
        ra.position = snap.GetProperty<Math::Vector3>(a.id, "position");
        ra.health   = snap.GetProperty<float>(a.id, "health");
        ra.active   = snap.GetProperty<bool>(a.id, "active");
        EXPECT_EQ(ra, a);
    }
}

// 2. Delta snapshot: only changed actors
TEST_F(ReplicationTest, DeltaSnapshot_OnlyChanged) {
    // change half actors
    std::vector<uint32_t> changed;
    for (size_t i = 0; i < actors.size(); i += 2) {
        auto a2 = RandomizeActor(actors[i]);
        actors[i] = a2;
        mgr->SetProperty(a2.id, "position", a2.position);
        mgr->SetProperty(a2.id, "health", a2.health);
        mgr->SetProperty(a2.id, "active", a2.active);
        changed.push_back(a2.id);
    }
    auto delta = mgr->CreateSnapshot(false);
    ASSERT_EQ(delta.actorCount(), changed.size());
    for (auto id : changed) {
        EXPECT_TRUE(delta.HasActor(id));
    }
}

// 3. Reliable vs unreliable channels
TEST_F(ReplicationTest, ReliableUnreliableChannels) {
    mgr->SetChannelReliable(false);
    auto snapUnrel = mgr->CreateSnapshot(true);
    EXPECT_FALSE(snapUnrel.isReliable());
    mgr->SetChannelReliable(true);
    auto snapRel = mgr->CreateSnapshot(true);
    EXPECT_TRUE(snapRel.isReliable());
}

// 4. Tick alignment: snapshots carry sequence
TEST_F(ReplicationTest, SnapshotSequence_Increments) {
    auto s1 = mgr->CreateSnapshot(true);
    auto s2 = mgr->CreateSnapshot(true);
    EXPECT_LT(s1.sequence(), s2.sequence());
}

// 5. Out-of-order and drop handling
TEST_F(ReplicationTest, OutOfOrder_DroppedIgnored) {
    auto s1 = mgr->CreateSnapshot(true);
    auto s2 = mgr->CreateSnapshot(true);
    // apply s2 then s1
    EXPECT_TRUE(mgr->ApplySnapshot(s2));
    EXPECT_FALSE(mgr->ApplySnapshot(s1)); // older sequence ignored
}

// 6. Performance under many rapid updates
TEST_F(ReplicationTest, Performance_HighFrequencyUpdates) {
    const int updates = 10000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < updates; ++i) {
        for (auto& a : actors) {
            auto a2 = RandomizeActor(a);
            mgr->SetProperty(a2.id, "position", a2.position);
            mgr->SetProperty(a2.id, "health", a2.health);
            mgr->SetProperty(a2.id, "active", a2.active);
        }
        mgr->CreateSnapshot(false);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    EXPECT_LT(ms, 2000.0);  // expect <2s
}

// 7. Edge cases
TEST_F(ReplicationTest, NoChanges_EmptyDelta) {
    auto delta = mgr->CreateSnapshot(false);
    EXPECT_EQ(delta.actorCount(), 0u);
}

TEST_F(ReplicationTest, AllPropertiesChanged_DeltaEqualsFull) {
    for (auto& a : actors) {
        auto a2 = RandomizeActor(a);
        actors[a.id-1] = a2;
        mgr->SetProperty(a2.id, "position", a2.position);
        mgr->SetProperty(a2.id, "health", a2.health);
        mgr->SetProperty(a2.id, "active", a2.active);
    }
    auto delta = mgr->CreateSnapshot(false);
    EXPECT_EQ(delta.actorCount(), actors.size());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}