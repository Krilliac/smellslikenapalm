#pragma once

#include <vector>
#include <memory>
#include "Math/Vector3.h"
#include "Game/ParticleEffect.h"

struct RigidBody {
    uint32_t id;
    Vector3 position;
    Vector3 velocity;
    Vector3 acceleration;
    float   mass;
    bool    isStatic;
};

class PhysicsEngine {
public:
    PhysicsEngine();
    ~PhysicsEngine();

    // Initialize physics world (gravity, timestep)
    void Initialize(const Vector3& gravity, float timeStep);

    // Add/remove rigid bodies
    uint32_t AddRigidBody(const RigidBody& body);
    void     RemoveRigidBody(uint32_t bodyId);

    // Per-frame update
    void     Update();

    // Raycast query
    bool     Raycast(const Vector3& origin,
                     const Vector3& direction,
                     float maxDistance,
                     uint32_t& outBodyId,
                     Vector3& outHitPoint);

    // Collision callbacks
    using CollisionCallback = std::function<void(uint32_t a, uint32_t b)>;
    void     SetCollisionCallback(CollisionCallback cb);

    // Particle effects integration
    void     TriggerParticle(const ParticleEffect& effect, const Vector3& position);

private:
    Vector3                 m_gravity;
    float                   m_timeStep;
    std::vector<RigidBody>  m_bodies;
    std::unordered_map<uint32_t, size_t> m_bodyIndex;
    CollisionCallback       m_collisionCb;

    void     Integrate(RigidBody& body);
    void     DetectCollisions();
};