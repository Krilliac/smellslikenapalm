// tests/PhysicsTests.cpp
// Comprehensive physics and collision subsystem unit tests
//
// Tests cover:
// 1. PhysicsEngine initialization, body creation/destruction.
// 2. Rigid body dynamics: position, velocity, gravity.
// 3. Collision detection: sphere-sphere, box-box, sphere-box.
// 4. Raycasting: hit/miss, closest hit, normals.
// 5. Integration with MapManager for geometry collisions.
// 6. Performance under many bodies.
// 7. Edge cases: zero-size bodies, out-of-bounds queries.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <random>
#include <chrono>

#include "Physics/PhysicsEngine.h"
#include "Physics/CollisionDetection.h"
#include "Physics/BoundingVolume.h"
#include "Game/MapManager.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"

using ::testing::_;

// Fixture for PhysicsEngine tests
class PhysicsEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        phys = std::make_unique<PhysicsEngine>();
        ASSERT_TRUE(phys->Initialize());
    }
    void TearDown() override {
        phys->Shutdown();
    }
    std::unique_ptr<PhysicsEngine> phys;
};

// 1. Initialization and body lifecycle
TEST_F(PhysicsEngineTest, Initialize_CreateDestroyBodies) {
    uint32_t id1 = phys->CreateRigidBody({0,0,0}, {1,1,1}, 1.0f);
    uint32_t id2 = phys->CreateRigidBody({10,0,0}, {2,2,2}, 2.0f);
    EXPECT_NE(id1, id2);
    phys->DestroyRigidBody(id1);
    phys->DestroyRigidBody(id2);
}

// 2. Dynamics under gravity
TEST_F(PhysicsEngineTest, Gravity_AppliesToDynamicBody) {
    uint32_t id = phys->CreateRigidBody({0,10,0}, {1,1,1}, 1.0f);
    phys->SetVelocity(id, {0,0,0});
    phys->Update(1.0f);  // 1 second
    Vector3 pos = phys->GetPosition(id);
    EXPECT_LT(pos.y, 10.0f); // Should have fallen
    phys->DestroyRigidBody(id);
}

// 3. Sphere-sphere collision
TEST(CollisionDetectionTest, SphereVsSphere_CollisionAndNoCollision) {
    CollisionObject s1(1, {0,0,0}, CollisionShape::SPHERE);
    s1.radius = 1.0f;
    CollisionObject s2(2, {1.5f,0,0}, CollisionShape::SPHERE);
    s2.radius = 1.0f;
    CollisionResult res;
    EXPECT_TRUE(CollisionDetector::SphereVsSphere(s1, s2, res));
    s2.position = {3.0f,0,0};
    EXPECT_FALSE(CollisionDetector::SphereVsSphere(s1, s2, res));
}

// 4. Box-box collision
TEST(CollisionDetectionTest, BoxVsBox_CollisionAndNoCollision) {
    CollisionObject b1(1, {0,0,0}, CollisionShape::BOX);
    b1.size = {2,2,2};
    CollisionObject b2(2, {1,0,0}, CollisionShape::BOX);
    b2.size = {2,2,2};
    CollisionResult res;
    // Box-box uses AABB overlap
    EXPECT_TRUE(phys::AABBvsAABB(b1, b2));
    b2.position = {5,0,0};
    EXPECT_FALSE(phys::AABBvsAABB(b1, b2));
}

// 5. Sphere-box collision
TEST(CollisionDetectionTest, SphereVsBox_CollisionAndNoCollision) {
    CollisionObject sphere(1, {0,0,0}, CollisionShape::SPHERE);
    sphere.radius = 1.0f;
    CollisionObject box(2, {1.5f,0,0}, CollisionShape::BOX);
    box.size = {1,1,1};
    CollisionResult res;
    EXPECT_TRUE(CollisionDetector::SphereVsBox(sphere, box, res));
    box.position = {5,0,0};
    EXPECT_FALSE(CollisionDetector::SphereVsBox(sphere, box, res));
}

// 6. Raycasting in world
TEST(RaycastTest, RayvsSphere_HitAndMiss) {
    CollisionObject sphere(1, {5,0,0}, CollisionShape::SPHERE);
    sphere.radius = 1.0f;
    RaycastResult r1 = CollisionDetector::RayVsSphere({0,0,0}, {1,0,0}, sphere);
    EXPECT_TRUE(r1.hit);
    EXPECT_NEAR(r1.distance, 4.0f, 1e-3f);
    RaycastResult r2 = CollisionDetector::RayVsSphere({0,0,0}, {0,1,0}, sphere);
    EXPECT_FALSE(r2.hit);
}

// 7. Map geometry collision
TEST(MapManagerTest, LineOfSight_BlockedAndClear) {
    MapManager mgr;
    mgr.AddStaticBox({0,0,5}, {10,10,1}); // wall at z=5
    EXPECT_FALSE(mgr.LineOfSight({0,0,0}, {0,0,10}));
    EXPECT_TRUE(mgr.LineOfSight({0,0,0}, {10,0,0}));
}

// 8. Performance under many bodies
TEST_F(PhysicsEngineTest, Performance_ManyBodies_Update) {
    const int N = 1000;
    std::vector<uint32_t> bodies;
    for(int i=0;i<N;i++){
        bodies.push_back(phys->CreateRigidBody({float(i),0,0},{1,1,1},1.0f));
    }
    auto start = std::chrono::high_resolution_clock::now();
    phys->Update(1.0f/60.0f);
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(end-start).count();
    EXPECT_LT(ms, 50.0); // update 1000 bodies in <50ms
    for(auto id:bodies) phys->DestroyRigidBody(id);
}

// 9. Edge cases: zero-size body
TEST_F(PhysicsEngineTest, ZeroSizeBody_HandledSafely) {
    uint32_t id = phys->CreateRigidBody({0,0,0},{0,0,0},1.0f);
    EXPECT_NO_THROW(phys->Update(1.0f/60.0f));
    phys->DestroyRigidBody(id);
}

// 10. Out-of-bounds raycast
TEST(FastRaycastTest, Raycast_NoBodies_ReturnsNoHit) {
    MapManager mgr;
    auto res = mgr.Raycast({0,0,0}, {1,0,0}, 100.0f);
    EXPECT_FALSE(res.hit);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}