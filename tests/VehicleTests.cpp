// tests/VehicleTests.cpp
// Comprehensive unit tests for vehicle simulation and handling
//
// Covers:
// 1. Vehicle creation and registration.
// 2. Movement physics: acceleration, braking, steering.
// 3. Collision with world geometry.
// 4. Network replication of vehicle state.
// 5. Health/damage and destruction.
// 6. Performance under many vehicles.
// 7. Edge cases: zero velocity, max speed, invalid inputs.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "Game/VehicleManager.h"
#include "Physics/PhysicsEngine.h"
#include "Math/Vector3.h"
#include "Network/ReplicationManager.h"
#include "Utils/Logger.h"

using ::testing::_;

// Fixture for VehicleManager tests
class VehicleTests : public ::testing::Test {
protected:
    void SetUp() override {
        phys = std::make_unique<PhysicsEngine>();
        ASSERT_TRUE(phys->Initialize());
        vm = std::make_unique<VehicleManager>(phys.get());
        rm = std::make_unique<ReplicationManager>();
    }
    void TearDown() override {
        vm.reset();
        phys->Shutdown();
        phys.reset();
        rm.reset();
    }
    std::unique_ptr<PhysicsEngine> phys;
    std::unique_ptr<VehicleManager> vm;
    std::unique_ptr<ReplicationManager> rm;
};

// 1. Creation and registration
TEST_F(VehicleTests, CreateVehicle_AssignsUniqueId) {
    uint32_t v1 = vm->CreateVehicle("CarModel", {0,0,0});
    uint32_t v2 = vm->CreateVehicle("TruckModel", {10,0,0});
    EXPECT_NE(v1, v2);
    EXPECT_TRUE(vm->IsValidVehicle(v1));
    EXPECT_TRUE(vm->IsValidVehicle(v2));
}

// 2. Acceleration and max speed
TEST_F(VehicleTests, Acceleration_RespectsMaxSpeed) {
    uint32_t vid = vm->CreateVehicle("Sport", {0,0,0});
    float maxSpeed = vm->GetMaxSpeed(vid);
    // apply acceleration repeatedly
    for (int i = 0; i < 100; ++i) vm->ApplyThrottle(vid, 1.0f, 1.0f/60.0f);
    Vector3 vel = vm->GetVelocity(vid);
    EXPECT_LE(vel.Length(), maxSpeed + 1e-2f);
}

// 3. Braking to stop
TEST_F(VehicleTests, Braking_ToZeroVelocity) {
    uint32_t vid = vm->CreateVehicle("SUV", {0,0,0});
    vm->SetVelocity(vid, {20,0,0});
    while (vm->GetVelocity(vid).Length() > 0.1f) {
        vm->ApplyBrake(vid, 1.0f, 1.0f/60.0f);
    }
    EXPECT_LE(vm->GetVelocity(vid).Length(), 0.1f);
}

// 4. Steering changes direction
TEST_F(VehicleTests, Steering_AdjustsHeading) {
    uint32_t vid = vm->CreateVehicle("Car", {0,0,0});
    vm->SetVelocity(vid, {10,0,0});
    float beforeYaw = vm->GetYaw(vid);
    vm->ApplySteering(vid, 1.0f, 1.0f/60.0f);
    EXPECT_NE(vm->GetYaw(vid), beforeYaw);
}

// 5. Collision with environment
TEST_F(VehicleTests, Collision_WithStaticObstacle) {
    // create wall at x=5
    phys->CreateRigidBody({5,0,0}, {1,5,10}, 0.0f);
    uint32_t vid = vm->CreateVehicle("Car", {0,0,0});
    vm->SetVelocity(vid, {10,0,0});
    // simulate until collision
    bool collided = false;
    for (int i = 0; i < 600; ++i) {
        phys->Update(1.0f/60.0f);
        vm->Update(1.0f/60.0f);
        if (vm->HasCollided(vid)) { collided = true; break; }
    }
    EXPECT_TRUE(collided);
}

// 6. Replication of state
TEST_F(VehicleTests, Replication_StateRoundTrip) {
    uint32_t vid = vm->CreateVehicle("Car", {1,2,3});
    vm->SetHealth(vid, 75);
    vm->SetVelocity(vid, {5,0,0});
    // register with replication
    rm->RegisterVehicle(vid);
    auto snap = rm->CreateVehicleSnapshot();
    EXPECT_TRUE(snap.HasVehicle(vid));
    auto state = snap.GetVehicleState(vid);
    EXPECT_EQ(state.health, 75);
    EXPECT_EQ(state.position, Vector3(1,2,3));
    EXPECT_EQ(state.velocity, Vector3(5,0,0));
}

// 7. Damage and destruction
TEST_F(VehicleTests, DamageAndDestroy) {
    uint32_t vid = vm->CreateVehicle("Tank", {0,0,0});
    float hp = vm->GetHealth(vid);
    vm->ApplyDamage(vid, hp + 10);
    EXPECT_FALSE(vm->IsValidVehicle(vid));
}

// 8. Performance under load
TEST_F(VehicleTests, Performance_ManyVehicles_Update) {
    const int N = 500;
    std::vector<uint32_t> vids;
    for (int i = 0; i < N; ++i)
        vids.push_back(vm->CreateVehicle("Car", {float(i),0,0}));
    auto start = std::chrono::high_resolution_clock::now();
    for (int frame = 0; frame < 60; ++frame) {
        phys->Update(1.0f/60.0f);
        vm->Update(1.0f/60.0f);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(end - start).count();
    EXPECT_LT(ms, 100.0);  // 60 frames <100ms
}

// 9. Edge: zero throttle yields constant velocity
TEST_F(VehicleTests, ZeroThrottle_MaintainsVelocity) {
    uint32_t vid = vm->CreateVehicle("Car", {0,0,0});
    vm->SetVelocity(vid, {15,0,0});
    Vector3 prev = vm->GetVelocity(vid);
    vm->UpdateVehiclePhysics(vid, 1.0f/60.0f, 0.0f, 0.0f);
    EXPECT_EQ(vm->GetVelocity(vid), prev);
}

// 10. Invalid inputs handled gracefully
TEST_F(VehicleTests, InvalidVehicleID_NoCrash) {
    EXPECT_NO_THROW(vm->ApplyThrottle(9999, 1.0f, 1.0f/60.0f));
    EXPECT_NO_THROW(vm->ApplyBrake(9999, 1.0f, 1.0f/60.0f));
    EXPECT_NO_THROW(vm->ApplySteering(9999, 1.0f, 1.0f/60.0f));
    EXPECT_NO_THROW(vm->GetHealth(9999));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}