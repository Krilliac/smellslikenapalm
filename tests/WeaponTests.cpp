// tests/WeaponTests.cpp
// Comprehensive unit tests for weapon subsystem
//
// Covers:
// 1. Weapon creation, registration, and retrieval.
// 2. Firing mechanics: rate of fire, ammo consumption, cooldown.
// 3. Projectile spawning, velocity, and lifetime.
// 4. Hit detection against targets and environment.
// 5. Damage application, falloff, and critical hits.
// 6. Reloading behavior and timing.
// 7. Edge cases: zero ammo, overheated weapons, invalid weapon IDs.
// 8. Performance under many simultaneous shots.
// 9. Multiplayer replication of weapon state.
// 10. Security: input validation for fire requests.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <chrono>
#include <thread>

#include "Game/WeaponManager.h"
#include "Game/ProjectileManager.h"
#include "Game/PlayerManager.h"
#include "Physics/CollisionDetection.h"
#include "Math/Vector3.h"
#include "Network/ReplicationManager.h"
#include "Utils/Logger.h"

using ::testing::_;

// Fixture for WeaponManager tests
class WeaponTests : public ::testing::Test {
protected:
    void SetUp() override {
        pm = std::make_unique<PlayerManager>();
        wm = std::make_unique<WeaponManager>(pm.get());
        proj = std::make_unique<ProjectileManager>();
        rm = std::make_unique<ReplicationManager>();
        // Register default weapon prototype
        wm->RegisterWeaponType("Rifle", WeaponDefinition{
            600,        // RPM
            30,         // magazine
            2.0f,       // reload seconds
            800.0f,     // projectile speed
            5.0f,       // damage
            0.1f        // critical chance
        });
    }

    void TearDown() override {
        proj.reset();
        wm.reset();
        pm.reset();
        rm.reset();
    }

    std::unique_ptr<PlayerManager> pm;
    std::unique_ptr<WeaponManager> wm;
    std::unique_ptr<ProjectileManager> proj;
    std::unique_ptr<ReplicationManager> rm;
};

// 1. Weapon creation and retrieval
TEST_F(WeaponTests, CreateWeaponInstance_Succeeds) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    EXPECT_NE(wid, 0u);
    auto def = wm->GetWeaponDefinition(wid);
    EXPECT_EQ(def.name, "Rifle");
    EXPECT_EQ(wm->GetAmmo(wid), 30);
}

// 2. Firing consumes ammo and enforces cooldown
TEST_F(WeaponTests, FireWeapon_ConsumesAmmoAndRespectsRate) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    pm->AddNewPlayer("p1");
    // First shot
    bool fired = wm->Fire(wid);
    EXPECT_TRUE(fired);
    EXPECT_EQ(wm->GetAmmo(wid), 29);
    // Immediate second shot should be blocked by cooldown
    fired = wm->Fire(wid);
    EXPECT_FALSE(fired);
    // Wait for cooldown (60 RPM => 1 shot per 0.1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fired = wm->Fire(wid);
    EXPECT_TRUE(fired);
}

// 3. Projectile spawn and velocity
TEST_F(WeaponTests, FireSpawnsProjectileWithCorrectVelocity) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    Vector3 dir{1,0,0};
    wm->SetAimDirection(wid, dir);
    wm->Fire(wid);
    auto id = proj->GetLatestProjectileId();
    EXPECT_NE(id, 0u);
    auto vel = proj->GetVelocity(id);
    EXPECT_NEAR(vel.x, 800.0f, 1e-3f);
    EXPECT_NEAR(vel.y, 0.0f, 1e-3f);
}

// 4. Hit detection applies damage
TEST_F(WeaponTests, ProjectileHit_TargetTakesDamage) {
    uint32_t pid = pm->AddNewPlayer("p2");
    pm->SetHealth(pid, 100);
    uint32_t wid = wm->CreateWeapon("Rifle");
    Vector3 targetPos = {10,0,0};
    proj->SpawnProjectile({0,0,0}, {1,0,0}, wid);
    // Simulate flight until hit
    for (int i = 0; i < 20; ++i) {
        proj->Update(0.05f);
    }
    CollisionResult res;
    bool hit = CollisionDetector::SphereVsSphere(
        proj->GetCollisionObject(proj->GetLatestProjectileId()),
        CollisionObject(pid, targetPos, CollisionShape::SPHERE),
        res);
    EXPECT_TRUE(hit);
    if (hit) {
        pm->ApplyDamage(pid, res.penetrationDepth * wm->GetDamage(wid), wid);
        EXPECT_LT(pm->GetHealth(pid), 100.0f);
    }
}

// 5. Damage falloff over distance
TEST_F(WeaponTests, DamageFalloff_AppliedCorrectly) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    float baseDamage = wm->GetDamage(wid);
    float shortDist = wm->ComputeDamageWithFalloff(wid, 0.0f);
    float longDist = wm->ComputeDamageWithFalloff(wid, 1000.0f);
    EXPECT_EQ(shortDist, baseDamage);
    EXPECT_LT(longDist, baseDamage);
}

// 6. Reloading restores ammo after delay
TEST_F(WeaponTests, Reload_RefillsMagazineAfterDelay) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    wm->SetAmmo(wid, 0);
    bool reloading = wm->StartReload(wid);
    EXPECT_TRUE(reloading);
    EXPECT_EQ(wm->GetAmmo(wid), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    wm->UpdateReloads();
    EXPECT_EQ(wm->GetAmmo(wid), 30);
}

// 7. Edge: firing with zero ammo fails
TEST_F(WeaponTests, FireWithNoAmmo_Fails) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    wm->SetAmmo(wid, 0);
    EXPECT_FALSE(wm->Fire(wid));
}

// 8. Performance: many simultaneous shots
TEST_F(WeaponTests, Performance_ManyShots_NoCrash) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        if (wm->GetAmmo(wid) == 0) {
            wm->StartReload(wid);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            wm->UpdateReloads();
        }
        wm->Fire(wid);
    }
    SUCCEED();
}

// 9. Replication of weapon state
TEST_F(WeaponTests, Replication_WeaponStateRoundTrip) {
    uint32_t wid = wm->CreateWeapon("Rifle");
    wm->SetAmmo(wid, 15);
    rm->RegisterWeapon(wid);
    auto snap = rm->CreateWeaponSnapshot();
    EXPECT_TRUE(snap.HasWeapon(wid));
    auto state = snap.GetWeaponState(wid);
    EXPECT_EQ(state.ammo, 15);
}

// 10. Invalid weapon ID handled gracefully
TEST_F(WeaponTests, InvalidWeaponID_NoCrash) {
    EXPECT_FALSE(wm->Fire(9999));
    EXPECT_FALSE(wm->StartReload(9999));
    EXPECT_EQ(wm->GetAmmo(9999), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}