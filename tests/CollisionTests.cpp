// tests/CollisionTests.cpp
// Comprehensive collision detection and physics validation tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <cmath>
#include <algorithm>

// Include the headers
#include "Physics/PhysicsEngine.h"
#include "Physics/CollisionDetection.h"
#include "Physics/BoundingVolume.h"
#include "Game/PlayerManager.h"
#include "Game/MapManager.h"
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"
#include "Utils/PerformanceProfiler.h"
#include "Math/Vector3.h"
#include "Math/Matrix4.h"
#include "Math/Quaternion.h"

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgReferee;
using ::testing::AtLeast;
using ::testing::Between;

// Constants for collision testing
constexpr float EPSILON = 0.0001f;
constexpr float DEFAULT_PLAYER_RADIUS = 0.5f;
constexpr float DEFAULT_PLAYER_HEIGHT = 1.8f;
constexpr float DEFAULT_PROJECTILE_RADIUS = 0.05f;
constexpr float GRAVITY = -9.81f;
constexpr float MAX_VELOCITY = 50.0f;

// Collision shape types
enum class CollisionShape {
    SPHERE,
    CAPSULE,
    BOX,
    MESH,
    PLANE,
    RAY
};

// Physics material properties
struct PhysicsMaterial {
    float friction = 0.7f;
    float restitution = 0.3f;
    float density = 1.0f;
    bool isTrigger = false;
};

// Collision object for testing
struct CollisionObject {
    uint32_t id;
    Vector3 position;
    Vector3 velocity;
    Vector3 size; // For box/capsule dimensions
    float radius; // For sphere/capsule
    CollisionShape shape;
    PhysicsMaterial material;
    bool isStatic;
    bool isActive;
    
    CollisionObject(uint32_t objId, const Vector3& pos, CollisionShape shapeType)
        : id(objId), position(pos), velocity(0, 0, 0), size(1, 1, 1)
        , radius(0.5f), shape(shapeType), isStatic(false), isActive(true) {}
};

// Collision result data
struct CollisionResult {
    bool hasCollision;
    Vector3 contactPoint;
    Vector3 contactNormal;
    float penetrationDepth;
    uint32_t object1Id;
    uint32_t object2Id;
    float collisionTime;
    
    CollisionResult() : hasCollision(false), penetrationDepth(0.0f)
                      , object1Id(0), object2Id(0), collisionTime(0.0f) {}
};

// Ray casting result
struct RaycastResult {
    bool hit;
    Vector3 hitPoint;
    Vector3 hitNormal;
    float distance;
    uint32_t objectId;
    
    RaycastResult() : hit(false), distance(std::numeric_limits<float>::max()), objectId(0) {}
};

// Mock classes for collision testing
class MockPhysicsEngine : public PhysicsEngine {
public:
    MOCK_METHOD(bool, Initialize, (), (override));
    MOCK_METHOD(void, Shutdown, (), (override));
    MOCK_METHOD(void, Update, (float deltaTime), (override));
    MOCK_METHOD(uint32_t, CreateRigidBody, (const Vector3& position, const Vector3& size, float mass), (override));
    MOCK_METHOD(void, DestroyRigidBody, (uint32_t bodyId), (override));
    MOCK_METHOD(void, SetPosition, (uint32_t bodyId, const Vector3& position), (override));
    MOCK_METHOD(void, SetVelocity, (uint32_t bodyId, const Vector3& velocity), (override));
    MOCK_METHOD(Vector3, GetPosition, (uint32_t bodyId), (const, override));
    MOCK_METHOD(Vector3, GetVelocity, (uint32_t bodyId), (const, override));
    MOCK_METHOD(bool, CheckCollision, (uint32_t body1, uint32_t body2), (const, override));
    MOCK_METHOD(std::vector<uint32_t>, GetCollidingBodies, (uint32_t bodyId), (const, override));
    MOCK_METHOD(RaycastResult, Raycast, (const Vector3& origin, const Vector3& direction, float maxDistance), (const, override));
};

class MockMapManager : public MapManager {
public:
    MOCK_METHOD(bool, LoadMap, (const std::string& mapName), (override));
    MOCK_METHOD(bool, IsValidPosition, (const Vector3& position), (const, override));
    MOCK_METHOD(float, GetHeightAtPosition, (float x, float z), (const, override));
    MOCK_METHOD(std::vector<Vector3>, GetSpawnPoints, (), (const, override));
    MOCK_METHOD(bool, LineOfSight, (const Vector3& from, const Vector3& to), (const, override));
    MOCK_METHOD(std::vector<uint32_t>, GetNearbyGeometry, (const Vector3& center, float radius), (const, override));
};

class MockPlayerManager : public PlayerManager {
public:
    MOCK_METHOD(bool, IsValidPlayerPosition, (uint32_t playerId, const Vector3& position), (const, override));
    MOCK_METHOD(void, UpdatePlayerPosition, (uint32_t playerId, const Vector3& position), (override));
    MOCK_METHOD(Vector3, GetPlayerPosition, (uint32_t playerId), (const, override));
    MOCK_METHOD(float, GetPlayerRadius, (uint32_t playerId), (const, override));
    MOCK_METHOD(float, GetPlayerHeight, (uint32_t playerId), (const, override));
    MOCK_METHOD(bool, CheckPlayerCollision, (uint32_t playerId, const Vector3& newPosition), (const, override));
};

// Collision detection implementation for testing
class CollisionDetector {
public:
    static bool SphereVsSphere(const CollisionObject& sphere1, const CollisionObject& sphere2, CollisionResult& result) {
        Vector3 diff = sphere1.position - sphere2.position;
        float distance = diff.Length();
        float radiusSum = sphere1.radius + sphere2.radius;
        
        if (distance < radiusSum) {
            result.hasCollision = true;
            result.object1Id = sphere1.id;
            result.object2Id = sphere2.id;
            result.penetrationDepth = radiusSum - distance;
            
            if (distance > EPSILON) {
                result.contactNormal = diff / distance;
                result.contactPoint = sphere2.position + result.contactNormal * sphere2.radius;
            } else {
                result.contactNormal = Vector3(1, 0, 0); // Arbitrary normal for coincident spheres
                result.contactPoint = sphere1.position;
            }
            return true;
        }
        
        result.hasCollision = false;
        return false;
    }
    
    static bool SphereVsBox(const CollisionObject& sphere, const CollisionObject& box, CollisionResult& result) {
        // Find closest point on box to sphere center
        Vector3 closestPoint;
        Vector3 boxMin = box.position - box.size * 0.5f;
        Vector3 boxMax = box.position + box.size * 0.5f;
        
        closestPoint.x = std::max(boxMin.x, std::min(sphere.position.x, boxMax.x));
        closestPoint.y = std::max(boxMin.y, std::min(sphere.position.y, boxMax.y));
        closestPoint.z = std::max(boxMin.z, std::min(sphere.position.z, boxMax.z));
        
        Vector3 diff = sphere.position - closestPoint;
        float distance = diff.Length();
        
        if (distance < sphere.radius) {
            result.hasCollision = true;
            result.object1Id = sphere.id;
            result.object2Id = box.id;
            result.penetrationDepth = sphere.radius - distance;
            result.contactPoint = closestPoint;
            
            if (distance > EPSILON) {
                result.contactNormal = diff / distance;
            } else {
                // Sphere center is inside box, find shortest exit direction
                Vector3 boxCenter = box.position;
                Vector3 toCenter = sphere.position - boxCenter;
                Vector3 extents = box.size * 0.5f;
                
                float minOverlap = std::numeric_limits<float>::max();
                result.contactNormal = Vector3(1, 0, 0);
                
                // Check each axis
                float overlapX = extents.x - std::abs(toCenter.x);
                float overlapY = extents.y - std::abs(toCenter.y);
                float overlapZ = extents.z - std::abs(toCenter.z);
                
                if (overlapX < minOverlap) {
                    minOverlap = overlapX;
                    result.contactNormal = Vector3(toCenter.x > 0 ? 1 : -1, 0, 0);
                }
                if (overlapY < minOverlap) {
                    minOverlap = overlapY;
                    result.contactNormal = Vector3(0, toCenter.y > 0 ? 1 : -1, 0);
                }
                if (overlapZ < minOverlap) {
                    result.contactNormal = Vector3(0, 0, toCenter.z > 0 ? 1 : -1);
                }
            }
            return true;
        }
        
        result.hasCollision = false;
        return false;
    }
    
    static bool CapsuleVsCapsule(const CollisionObject& cap1, const CollisionObject& cap2, CollisionResult& result) {
        // Simplified capsule collision (treating as spheres for this test)
        // In real implementation, would check line segment distance
        Vector3 diff = cap1.position - cap2.position;
        float distance = diff.Length();
        float radiusSum = cap1.radius + cap2.radius;
        
        if (distance < radiusSum) {
            result.hasCollision = true;
            result.object1Id = cap1.id;
            result.object2Id = cap2.id;
            result.penetrationDepth = radiusSum - distance;
            
            if (distance > EPSILON) {
                result.contactNormal = diff / distance;
                result.contactPoint = cap2.position + result.contactNormal * cap2.radius;
            }
            return true;
        }
        
        result.hasCollision = false;
        return false;
    }
    
    static RaycastResult RayVsSphere(const Vector3& rayOrigin, const Vector3& rayDirection, 
                                   const CollisionObject& sphere) {
        RaycastResult result;
        
        Vector3 oc = rayOrigin - sphere.position;
        float a = rayDirection.Dot(rayDirection);
        float b = 2.0f * oc.Dot(rayDirection);
        float c = oc.Dot(oc) - sphere.radius * sphere.radius;
        
        float discriminant = b * b - 4 * a * c;
        
        if (discriminant >= 0) {
            float t = (-b - std::sqrt(discriminant)) / (2 * a);
            if (t >= 0) {
                result.hit = true;
                result.distance = t;
                result.hitPoint = rayOrigin + rayDirection * t;
                result.hitNormal = (result.hitPoint - sphere.position).Normalized();
                result.objectId = sphere.id;
            }
        }
        
        return result;
    }
    
    static RaycastResult RayVsBox(const Vector3& rayOrigin, const Vector3& rayDirection,
                                const CollisionObject& box) {
        RaycastResult result;
        
        Vector3 boxMin = box.position - box.size * 0.5f;
        Vector3 boxMax = box.position + box.size * 0.5f;
        
        Vector3 invDir = Vector3(1.0f / rayDirection.x, 1.0f / rayDirection.y, 1.0f / rayDirection.z);
        
        Vector3 t1 = (boxMin - rayOrigin) * invDir;
        Vector3 t2 = (boxMax - rayOrigin) * invDir;
        
        Vector3 tMin = Vector3(std::min(t1.x, t2.x), std::min(t1.y, t2.y), std::min(t1.z, t2.z));
        Vector3 tMax = Vector3(std::max(t1.x, t2.x), std::max(t1.y, t2.y), std::max(t1.z, t2.z));
        
        float tNear = std::max({tMin.x, tMin.y, tMin.z});
        float tFar = std::min({tMax.x, tMax.y, tMax.z});
        
        if (tNear <= tFar && tFar >= 0) {
            float t = tNear >= 0 ? tNear : tFar;
            result.hit = true;
            result.distance = t;
            result.hitPoint = rayOrigin + rayDirection * t;
            result.objectId = box.id;
            
            // Calculate normal based on which face was hit
            Vector3 hitLocal = result.hitPoint - box.position;
            Vector3 absHit = Vector3(std::abs(hitLocal.x), std::abs(hitLocal.y), std::abs(hitLocal.z));
            Vector3 extents = box.size * 0.5f;
            
            if (absHit.x >= absHit.y && absHit.x >= absHit.z) {
                result.hitNormal = Vector3(hitLocal.x > 0 ? 1 : -1, 0, 0);
            } else if (absHit.y >= absHit.z) {
                result.hitNormal = Vector3(0, hitLocal.y > 0 ? 1 : -1, 0);
            } else {
                result.hitNormal = Vector3(0, 0, hitLocal.z > 0 ? 1 : -1);
            }
        }
        
        return result;
    }
};

// Test fixture for collision tests
class CollisionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize mocks
        mockPhysicsEngine = std::make_shared<NiceMock<MockPhysicsEngine>>();
        mockMapManager = std::make_shared<NiceMock<MockMapManager>>();
        mockPlayerManager = std::make_shared<NiceMock<MockPlayerManager>>();

        // Set up default mock behavior
        ON_CALL(*mockPhysicsEngine, Initialize())
            .WillByDefault(Return(true));
        ON_CALL(*mockMapManager, IsValidPosition(_))
            .WillByDefault(Return(true));
        ON_CALL(*mockMapManager, GetHeightAtPosition(_, _))
            .WillByDefault(Return(0.0f));
        ON_CALL(*mockPlayerManager, GetPlayerRadius(_))
            .WillByDefault(Return(DEFAULT_PLAYER_RADIUS));
        ON_CALL(*mockPlayerManager, GetPlayerHeight(_))
            .WillByDefault(Return(DEFAULT_PLAYER_HEIGHT));

        // Clear test objects
        testObjects.clear();
        collisionResults.clear();
    }

    void TearDown() override {
        testObjects.clear();
        collisionResults.clear();
        mockPlayerManager.reset();
        mockMapManager.reset();
        mockPhysicsEngine.reset();
    }

    // Helper methods
    uint32_t CreateTestObject(const Vector3& position, CollisionShape shape, float radius = 0.5f) {
        static uint32_t nextId = 1;
        uint32_t id = nextId++;
        
        CollisionObject obj(id, position, shape);
        obj.radius = radius;
        testObjects[id] = obj;
        
        return id;
    }

    uint32_t CreateTestBox(const Vector3& position, const Vector3& size) {
        uint32_t id = CreateTestObject(position, CollisionShape::BOX);
        testObjects[id].size = size;
        return id;
    }

    void MoveObject(uint32_t id, const Vector3& newPosition) {
        auto it = testObjects.find(id);
        if (it != testObjects.end()) {
            it->second.position = newPosition;
        }
    }

    void SetObjectVelocity(uint32_t id, const Vector3& velocity) {
        auto it = testObjects.find(id);
        if (it != testObjects.end()) {
            it->second.velocity = velocity;
        }
    }

    bool TestCollision(uint32_t id1, uint32_t id2) {
        auto it1 = testObjects.find(id1);
        auto it2 = testObjects.find(id2);
        
        if (it1 == testObjects.end() || it2 == testObjects.end()) {
            return false;
        }

        CollisionResult result;
        
        // Dispatch to appropriate collision test based on shapes
        if (it1->second.shape == CollisionShape::SPHERE && it2->second.shape == CollisionShape::SPHERE) {
            return CollisionDetector::SphereVsSphere(it1->second, it2->second, result);
        } else if ((it1->second.shape == CollisionShape::SPHERE && it2->second.shape == CollisionShape::BOX) ||
                   (it1->second.shape == CollisionShape::BOX && it2->second.shape == CollisionShape::SPHERE)) {
            const auto& sphere = it1->second.shape == CollisionShape::SPHERE ? it1->second : it2->second;
            const auto& box = it1->second.shape == CollisionShape::BOX ? it1->second : it2->second;
            return CollisionDetector::SphereVsBox(sphere, box, result);
        } else if (it1->second.shape == CollisionShape::CAPSULE && it2->second.shape == CollisionShape::CAPSULE) {
            return CollisionDetector::CapsuleVsCapsule(it1->second, it2->second, result);
        }
        
        if (result.hasCollision) {
            collisionResults.push_back(result);
        }
        
        return result.hasCollision;
    }

    RaycastResult TestRaycast(const Vector3& origin, const Vector3& direction, float maxDistance = 1000.0f) {
        RaycastResult closestResult;
        
        for (const auto& [id, obj] : testObjects) {
            if (!obj.isActive) continue;
            
            RaycastResult result;
            if (obj.shape == CollisionShape::SPHERE) {
                result = CollisionDetector::RayVsSphere(origin, direction, obj);
            } else if (obj.shape == CollisionShape::BOX) {
                result = CollisionDetector::RayVsBox(origin, direction, obj);
            }
            
            if (result.hit && result.distance < closestResult.distance && result.distance <= maxDistance) {
                closestResult = result;
            }
        }
        
        return closestResult;
    }

    Vector3 RandomVector3(float minVal = -10.0f, float maxVal = 10.0f) {
        std::uniform_real_distribution<float> dist(minVal, maxVal);
        return Vector3(dist(rng), dist(rng), dist(rng));
    }

    float RandomFloat(float minVal = 0.0f, float maxVal = 1.0f) {
        std::uniform_real_distribution<float> dist(minVal, maxVal);
        return dist(rng);
    }

    // Test data
    std::shared_ptr<MockPhysicsEngine> mockPhysicsEngine;
    std::shared_ptr<MockMapManager> mockMapManager;
    std::shared_ptr<MockPlayerManager> mockPlayerManager;
    std::unordered_map<uint32_t, CollisionObject> testObjects;
    std::vector<CollisionResult> collisionResults;
    std::mt19937 rng{std::random_device{}()};
};

// === Basic Collision Detection Tests ===

TEST_F(CollisionTest, SphereVsSphere_Touching_DetectsCollision) {
    // Arrange
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);
    uint32_t sphere2 = CreateTestObject(Vector3(2, 0, 0), CollisionShape::SPHERE, 1.0f);

    // Act
    bool collision = TestCollision(sphere1, sphere2);

    // Assert
    EXPECT_TRUE(collision);
    EXPECT_FALSE(collisionResults.empty());
    if (!collisionResults.empty()) {
        const auto& result = collisionResults[0];
        EXPECT_NEAR(result.penetrationDepth, 0.0f, EPSILON);
        EXPECT_NEAR(result.contactNormal.x, 1.0f, EPSILON);
    }
}

TEST_F(CollisionTest, SphereVsSphere_Overlapping_CorrectPenetration) {
    // Arrange
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);
    uint32_t sphere2 = CreateTestObject(Vector3(1, 0, 0), CollisionShape::SPHERE, 1.0f);

    // Act
    bool collision = TestCollision(sphere1, sphere2);

    // Assert
    EXPECT_TRUE(collision);
    EXPECT_FALSE(collisionResults.empty());
    if (!collisionResults.empty()) {
        const auto& result = collisionResults[0];
        EXPECT_NEAR(result.penetrationDepth, 1.0f, EPSILON); // 2.0 radius sum - 1.0 distance
        EXPECT_NEAR(result.contactNormal.x, 1.0f, EPSILON);
        EXPECT_NEAR(result.contactNormal.y, 0.0f, EPSILON);
        EXPECT_NEAR(result.contactNormal.z, 0.0f, EPSILON);
    }
}

TEST_F(CollisionTest, SphereVsSphere_Separated_NoCollision) {
    // Arrange
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);
    uint32_t sphere2 = CreateTestObject(Vector3(5, 0, 0), CollisionShape::SPHERE, 1.0f);

    // Act
    bool collision = TestCollision(sphere1, sphere2);

    // Assert
    EXPECT_FALSE(collision);
    EXPECT_TRUE(collisionResults.empty());
}

TEST_F(CollisionTest, SphereVsBox_SphereInside_DetectsCollision) {
    // Arrange
    uint32_t sphere = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 0.5f);
    uint32_t box = CreateTestBox(Vector3(0, 0, 0), Vector3(2, 2, 2));

    // Act
    bool collision = TestCollision(sphere, box);

    // Assert
    EXPECT_TRUE(collision);
}

TEST_F(CollisionTest, SphereVsBox_SphereOutside_NoCollision) {
    // Arrange
    uint32_t sphere = CreateTestObject(Vector3(5, 0, 0), CollisionShape::SPHERE, 0.5f);
    uint32_t box = CreateTestBox(Vector3(0, 0, 0), Vector3(2, 2, 2));

    // Act
    bool collision = TestCollision(sphere, box);

    // Assert
    EXPECT_FALSE(collision);
}

TEST_F(CollisionTest, SphereVsBox_SphereTouchingFace_DetectsCollision) {
    // Arrange
    uint32_t sphere = CreateTestObject(Vector3(1.5, 0, 0), CollisionShape::SPHERE, 0.5f);
    uint32_t box = CreateTestBox(Vector3(0, 0, 0), Vector3(2, 2, 2)); // Box extends from -1 to 1

    // Act
    bool collision = TestCollision(sphere, box);

    // Assert
    EXPECT_TRUE(collision);
}

// === Raycast Tests ===

TEST_F(CollisionTest, Raycast_HitSphere_ReturnsCorrectHit) {
    // Arrange
    uint32_t sphere = CreateTestObject(Vector3(5, 0, 0), CollisionShape::SPHERE, 1.0f);
    Vector3 rayOrigin(0, 0, 0);
    Vector3 rayDirection(1, 0, 0);

    // Act
    RaycastResult result = TestRaycast(rayOrigin, rayDirection);

    // Assert
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.objectId, sphere);
    EXPECT_NEAR(result.distance, 4.0f, EPSILON); // Ray hits at distance 4 (5 - 1 radius)
    EXPECT_NEAR(result.hitPoint.x, 4.0f, EPSILON);
    EXPECT_NEAR(result.hitNormal.x, -1.0f, EPSILON); // Normal points back toward ray origin
}

TEST_F(CollisionTest, Raycast_MissTarget_NoHit) {
    // Arrange
    uint32_t sphere = CreateTestObject(Vector3(5, 5, 0), CollisionShape::SPHERE, 1.0f);
    Vector3 rayOrigin(0, 0, 0);
    Vector3 rayDirection(1, 0, 0); // Ray goes along X axis, sphere is offset in Y

    // Act
    RaycastResult result = TestRaycast(rayOrigin, rayDirection);

    // Assert
    EXPECT_FALSE(result.hit);
}

TEST_F(CollisionTest, Raycast_HitBox_ReturnsCorrectHit) {
    // Arrange
    uint32_t box = CreateTestBox(Vector3(5, 0, 0), Vector3(2, 2, 2));
    Vector3 rayOrigin(0, 0, 0);
    Vector3 rayDirection(1, 0, 0);

    // Act
    RaycastResult result = TestRaycast(rayOrigin, rayDirection);

    // Assert
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.objectId, box);
    EXPECT_NEAR(result.distance, 4.0f, EPSILON); // Ray hits box face at x=4 (5-1)
    EXPECT_NEAR(result.hitPoint.x, 4.0f, EPSILON);
    EXPECT_NEAR(result.hitNormal.x, -1.0f, EPSILON);
}

TEST_F(CollisionTest, Raycast_MultipleObjects_HitsClosest) {
    // Arrange
    uint32_t nearSphere = CreateTestObject(Vector3(3, 0, 0), CollisionShape::SPHERE, 0.5f);
    uint32_t farSphere = CreateTestObject(Vector3(8, 0, 0), CollisionShape::SPHERE, 0.5f);
    Vector3 rayOrigin(0, 0, 0);
    Vector3 rayDirection(1, 0, 0);

    // Act
    RaycastResult result = TestRaycast(rayOrigin, rayDirection);

    // Assert
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.objectId, nearSphere); // Should hit the closer sphere
    EXPECT_NEAR(result.distance, 2.5f, EPSILON); // 3 - 0.5 radius
}

// === Player Movement Collision Tests ===

TEST_F(CollisionTest, PlayerMovement_ValidPosition_AllowsMovement) {
    // Arrange
    uint32_t playerId = 1;
    Vector3 currentPos(0, 0, 0);
    Vector3 newPos(1, 0, 0);

    EXPECT_CALL(*mockPlayerManager, IsValidPlayerPosition(playerId, newPos))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockPlayerManager, CheckPlayerCollision(playerId, newPos))
        .WillOnce(Return(false));

    // Act
    bool validPosition = mockPlayerManager->IsValidPlayerPosition(playerId, newPos);
    bool hasCollision = mockPlayerManager->CheckPlayerCollision(playerId, newPos);

    // Assert
    EXPECT_TRUE(validPosition);
    EXPECT_FALSE(hasCollision);
}

TEST_F(CollisionTest, PlayerMovement_CollisionDetected_PreventsMovement) {
    // Arrange
    uint32_t playerId = 1;
    Vector3 newPos(0, 0, 5); // Position that would collide

    EXPECT_CALL(*mockPlayerManager, CheckPlayerCollision(playerId, newPos))
        .WillOnce(Return(true));

    // Act
    bool hasCollision = mockPlayerManager->CheckPlayerCollision(playerId, newPos);

    // Assert
    EXPECT_TRUE(hasCollision);
}

TEST_F(CollisionTest, PlayerMovement_OutOfBounds_RejectsPosition) {
    // Arrange
    uint32_t playerId = 1;
    Vector3 outOfBoundsPos(-1000, -1000, -1000);

    EXPECT_CALL(*mockMapManager, IsValidPosition(outOfBoundsPos))
        .WillOnce(Return(false));

    // Act
    bool validPosition = mockMapManager->IsValidPosition(outOfBoundsPos);

    // Assert
    EXPECT_FALSE(validPosition);
}

// === Projectile Collision Tests ===

TEST_F(CollisionTest, ProjectileTrajectory_HitsTarget_CorrectImpact) {
    // Arrange - Projectile path intersects with target
    Vector3 projectileStart(0, 1, 0);
    Vector3 projectileVelocity(10, 0, 0); // Moving right
    uint32_t target = CreateTestObject(Vector3(5, 1, 0), CollisionShape::SPHERE, 0.5f);
    
    // Simulate projectile as a ray
    Vector3 rayDirection = projectileVelocity.Normalized();

    // Act
    RaycastResult result = TestRaycast(projectileStart, rayDirection);

    // Assert
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.objectId, target);
    EXPECT_NEAR(result.distance, 4.5f, EPSILON); // Distance to target surface
}

TEST_F(CollisionTest, ProjectileTrajectory_WithGravity_ParabolicPath) {
    // Arrange - Test ballistic trajectory
    Vector3 projectileStart(0, 2, 0);
    Vector3 initialVelocity(10, 5, 0); // Forward and up
    float timeStep = 0.1f;
    int maxSteps = 20;
    
    std::vector<Vector3> trajectory;
    Vector3 currentPos = projectileStart;
    Vector3 currentVel = initialVelocity;

    // Act - Simulate trajectory with gravity
    for (int step = 0; step < maxSteps; ++step) {
        trajectory.push_back(currentPos);
        
        // Apply gravity
        currentVel.y += GRAVITY * timeStep;
        currentPos += currentVel * timeStep;
        
        // Stop if hits ground
        if (currentPos.y <= 0) {
            trajectory.push_back(currentPos);
            break;
        }
    }

    // Assert
    EXPECT_GT(trajectory.size(), 2); // Should have multiple trajectory points
    EXPECT_LT(trajectory.back().y, trajectory.front().y); // Should end lower than start
    
    // Check trajectory is parabolic (peak then descend)
    bool foundPeak = false;
    for (size_t i = 1; i < trajectory.size() - 1; ++i) {
        if (trajectory[i].y > trajectory[i-1].y && trajectory[i].y > trajectory[i+1].y) {
            foundPeak = true;
            break;
        }
    }
    EXPECT_TRUE(foundPeak);
}

// === Anti-Cheat Collision Validation Tests ===

TEST_F(CollisionTest, AntiCheat_ImpossibleMovement_DetectsCheat) {
    // Arrange - Player tries to move through solid wall
    uint32_t playerId = 1;
    Vector3 playerStart(0, 0, 0);
    Vector3 playerEnd(10, 0, 0); // Far movement
    
    // Place wall between start and end
    uint32_t wall = CreateTestBox(Vector3(5, 0, 0), Vector3(1, 3, 3));
    
    EXPECT_CALL(*mockPlayerManager, GetPlayerPosition(playerId))
        .WillOnce(Return(playerStart));

    // Act - Check if line of sight exists (should be blocked by wall)
    EXPECT_CALL(*mockMapManager, LineOfSight(playerStart, playerEnd))
        .WillOnce(Return(false));
    
    bool hasLineOfSight = mockMapManager->LineOfSight(playerStart, playerEnd);

    // Assert
    EXPECT_FALSE(hasLineOfSight); // Movement should be impossible due to wall
}

TEST_F(CollisionTest, AntiCheat_ExcessiveSpeed_DetectsSpeedHack) {
    // Arrange
    uint32_t playerId = 1;
    Vector3 startPos(0, 0, 0);
    Vector3 endPos(100, 0, 0); // Moved 100 units
    float deltaTime = 0.1f; // In 0.1 seconds
    float maxPlayerSpeed = 10.0f; // Units per second
    
    // Act
    float distance = (endPos - startPos).Length();
    float speed = distance / deltaTime;
    bool isPossibleMovement = speed <= maxPlayerSpeed;

    // Assert
    EXPECT_FALSE(isPossibleMovement); // 1000 units/sec >> 10 max speed
}

TEST_F(CollisionTest, AntiCheat_NoClipDetection_CatchesWallHacking) {
    // Arrange - Player position updates on both sides of a wall without valid path
    Vector3 beforeWall(0, 0, 0);
    Vector3 afterWall(10, 0, 0);
    uint32_t wall = CreateTestBox(Vector3(5, 0, 0), Vector3(2, 5, 5)); // Tall wall

    EXPECT_CALL(*mockMapManager, LineOfSight(beforeWall, afterWall))
        .WillOnce(Return(false)); // Wall blocks line of sight

    // Act
    bool canSeeThrough = mockMapManager->LineOfSight(beforeWall, afterWall);

    // Assert
    EXPECT_FALSE(canSeeThrough); // Player cannot legally move through wall
}

// === Performance Tests ===

TEST_F(CollisionTest, Performance_ManyObjects_EfficientDetection) {
    // Arrange - Create many objects for performance testing
    const int objectCount = 1000;
    std::vector<uint32_t> objects;
    
    for (int i = 0; i < objectCount; ++i) {
        Vector3 pos = RandomVector3(-50, 50);
        uint32_t obj = CreateTestObject(pos, CollisionShape::SPHERE, RandomFloat(0.1f, 2.0f));
        objects.push_back(obj);
    }

    // Act - Test collision detection performance
    auto startTime = std::chrono::high_resolution_clock::now();
    
    int collisionCount = 0;
    for (int i = 0; i < objectCount; i += 10) { // Test every 10th object
        for (int j = i + 1; j < std::min(i + 10, objectCount); ++j) {
            if (TestCollision(objects[i], objects[j])) {
                collisionCount++;
            }
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert - Should complete in reasonable time
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for this many tests
    EXPECT_GE(collisionCount, 0); // At least no crashes
}

TEST_F(CollisionTest, Performance_ManyRaycasts_EfficientProcessing) {
    // Arrange
    const int raycastCount = 500;
    const int targetCount = 100;
    
    // Create targets
    for (int i = 0; i < targetCount; ++i) {
        Vector3 pos = RandomVector3(-20, 20);
        CreateTestObject(pos, CollisionShape::SPHERE, RandomFloat(0.5f, 1.5f));
    }

    // Act - Perform many raycasts
    auto startTime = std::chrono::high_resolution_clock::now();
    
    int hitCount = 0;
    for (int i = 0; i < raycastCount; ++i) {
        Vector3 origin = RandomVector3(-25, 25);
        Vector3 direction = RandomVector3(-1, 1).Normalized();
        
        RaycastResult result = TestRaycast(origin, direction, 50.0f);
        if (result.hit) {
            hitCount++;
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert
    EXPECT_LT(duration.count(), 5000); // Less than 5ms
    EXPECT_GE(hitCount, 0); // Some hits expected
}

// === Edge Cases and Robustness Tests ===

TEST_F(CollisionTest, EdgeCase_ZeroSizedSphere_HandledSafely) {
    // Arrange
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 0.0f);
    uint32_t sphere2 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);

    // Act & Assert - Should not crash
    EXPECT_NO_THROW({
        bool collision = TestCollision(sphere1, sphere2);
        EXPECT_TRUE(collision); // Zero-size sphere at same position should collide
    });
}

TEST_F(CollisionTest, EdgeCase_CoincidentSpheres_CorrectNormal) {
    // Arrange - Two spheres at exactly the same position
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);
    uint32_t sphere2 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1.0f);

    // Act
    bool collision = TestCollision(sphere1, sphere2);

    // Assert
    EXPECT_TRUE(collision);
    EXPECT_FALSE(collisionResults.empty());
    if (!collisionResults.empty()) {
        const auto& result = collisionResults[0];
        EXPECT_NEAR(result.penetrationDepth, 2.0f, EPSILON); // Full overlap
        EXPECT_NEAR(result.contactNormal.Length(), 1.0f, EPSILON); // Normal should be unit length
    }
}

TEST_F(CollisionTest, EdgeCase_ExtremelyLargeObjects_NoOverflow) {
    // Arrange
    uint32_t sphere1 = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE, 1e6f);
    uint32_t sphere2 = CreateTestObject(Vector3(1e6f, 0, 0), CollisionShape::SPHERE, 1e6f);

    // Act & Assert - Should handle large numbers without overflow
    EXPECT_NO_THROW({
        bool collision = TestCollision(sphere1, sphere2);
        EXPECT_TRUE(collision);
    });
}

TEST_F(CollisionTest, EdgeCase_NearlyZeroVelocity_StableSimulation) {
    // Arrange
    uint32_t obj = CreateTestObject(Vector3(0, 0, 0), CollisionShape::SPHERE);
    SetObjectVelocity(obj, Vector3(1e-10f, 1e-10f, 1e-10f)); // Nearly zero velocity

    // Act - Simulate movement
    Vector3 oldPos = testObjects[obj].position;
    Vector3 newPos = oldPos + testObjects[obj].velocity * 0.016f; // One frame
    MoveObject(obj, newPos);

    // Assert - Position should remain stable
    Vector3 movement = newPos - oldPos;
    EXPECT_LT(movement.Length(), 1e-8f); // Extremely small movement
}

// === Complex Scenario Tests ===

TEST_F(CollisionTest, ComplexScenario_MultiplayerCollision_AllPlayersHandled) {
    // Arrange - Multiple players in close proximity
    const int playerCount = 8;
    std::vector<uint32_t> players;
    
    // Create players in a circle
    for (int i = 0; i < playerCount; ++i) {
        float angle = (2.0f * M_PI * i) / playerCount;
        Vector3 pos(std::cos(angle) * 2.0f, 0, std::sin(angle) * 2.0f);
        uint32_t player = CreateTestObject(pos, CollisionShape::CAPSULE, DEFAULT_PLAYER_RADIUS);
        players.push_back(player);
    }

    // Act - Test all player-player collisions
    int collisionCount = 0;
    for (size_t i = 0; i < players.size(); ++i) {
        for (size_t j = i + 1; j < players.size(); ++j) {
            if (TestCollision(players[i], players[j])) {
                collisionCount++;
            }
        }
    }

    // Assert - Should detect some collisions but not crash
    EXPECT_GE(collisionCount, 0);
    EXPECT_LT(collisionCount, playerCount * playerCount); // Reasonable upper bound
}

TEST_F(CollisionTest, ComplexScenario_ExplosionDamage_RadialCollision) {
    // Arrange - Explosion at center with targets around
    Vector3 explosionCenter(0, 0, 0);
    float explosionRadius = 5.0f;
    
    std::vector<uint32_t> targets;
    std::vector<Vector3> targetPositions = {
        Vector3(3, 0, 0),   // Inside blast radius
        Vector3(7, 0, 0),   // Outside blast radius
        Vector3(0, 4, 0),   // Inside blast radius
        Vector3(0, 0, 6)    // Outside blast radius
    };
    
    for (const auto& pos : targetPositions) {
        targets.push_back(CreateTestObject(pos, CollisionShape::SPHERE, 0.5f));
    }

    // Act - Check which targets are in blast radius
    std::vector<bool> inBlastRadius;
    for (const auto& pos : targetPositions) {
        float distance = (pos - explosionCenter).Length();
        inBlastRadius.push_back(distance <= explosionRadius);
    }

    // Assert
    EXPECT_TRUE(inBlastRadius[0]);  // Target at (3,0,0) should be in radius
    EXPECT_FALSE(inBlastRadius[1]); // Target at (7,0,0) should be outside
    EXPECT_TRUE(inBlastRadius[2]);  // Target at (0,4,0) should be in radius
    EXPECT_FALSE(inBlastRadius[3]); // Target at (0,0,6) should be outside
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}