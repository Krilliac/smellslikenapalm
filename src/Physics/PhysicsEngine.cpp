#include "Physics/PhysicsEngine.h"
#include "Utils/Logger.h"
#include <algorithm>

PhysicsEngine::PhysicsEngine() = default;
PhysicsEngine::~PhysicsEngine() = default;

void PhysicsEngine::Initialize(const Vector3& gravity, float timeStep) {
    m_gravity = gravity;
    m_timeStep = timeStep;
    Logger::Info("PhysicsEngine initialized: gravity=(%.1f,%.1f,%.1f), dt=%.3f",
                 gravity.x, gravity.y, gravity.z, timeStep);
}

uint32_t PhysicsEngine::AddRigidBody(const RigidBody& body) {
    uint32_t id = body.id;
    m_bodyIndex[id] = m_bodies.size();
    m_bodies.push_back(body);
    return id;
}

void PhysicsEngine::RemoveRigidBody(uint32_t bodyId) {
    auto it = m_bodyIndex.find(bodyId);
    if (it == m_bodyIndex.end()) return;
    size_t idx = it->second, last = m_bodies.size()-1;
    std::swap(m_bodies[idx], m_bodies[last]);
    m_bodyIndex[m_bodies[idx].id] = idx;
    m_bodies.pop_back();
    m_bodyIndex.erase(bodyId);
}

void PhysicsEngine::Update() {
    for (auto& body : m_bodies) {
        if (!body.isStatic) Integrate(body);
    }
    DetectCollisions();
}

void PhysicsEngine::Integrate(RigidBody& body) {
    body.acceleration = m_gravity; 
    body.velocity += body.acceleration * m_timeStep;
    body.position += body.velocity * m_timeStep;
}

void PhysicsEngine::DetectCollisions() {
    for (size_t i=0; i<m_bodies.size(); ++i) {
        for (size_t j=i+1; j<m_bodies.size(); ++j) {
            auto& A = m_bodies[i], &B = m_bodies[j];
            float dist2 = (A.position - B.position).LengthSquared();
            float minDist = 1.0f; // assume unit radii
            if (dist2 < minDist*minDist) {
                if (m_collisionCb) m_collisionCb(A.id, B.id);
            }
        }
    }
}

bool PhysicsEngine::Raycast(const Vector3& origin,
                            const Vector3& direction,
                            float maxDistance,
                            uint32_t& outBodyId,
                            Vector3& outHitPoint)
{
    // Simple sphere intersection test
    for (auto& body : m_bodies) {
        Vector3 L = body.position - origin;
        float tca = L.Dot(direction);
        if (tca < 0) continue;
        float d2 = L.Dot(L) - tca*tca;
        float radius = 1.0f;
        if (d2 > radius*radius) continue;
        float thc = sqrt(radius*radius - d2);
        float t = tca - thc;
        if (t < 0) t = tca + thc;
        if (t > maxDistance) continue;
        outBodyId = body.id;
        outHitPoint = origin + direction * t;
        return true;
    }
    return false;
}

void PhysicsEngine::SetCollisionCallback(CollisionCallback cb) {
    m_collisionCb = std::move(cb);
}

void PhysicsEngine::TriggerParticle(const ParticleEffect& effect, const Vector3& position) {
    // Forward to game world or particle system
    Logger::Info("Triggering particle '%s' at (%.1f,%.1f,%.1f)",
                 effect.name.c_str(), position.x, position.y, position.z);
}