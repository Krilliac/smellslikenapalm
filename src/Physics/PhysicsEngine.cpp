#include "Physics/PhysicsEngine.h"
#include "Utils/Logger.h"
#include <algorithm>

PhysicsEngine::PhysicsEngine() {
    Logger::Trace("[PhysicsEngine::PhysicsEngine] Entry - constructing PhysicsEngine");
    Logger::Info("[PhysicsEngine::PhysicsEngine] PhysicsEngine instance created");
    Logger::Trace("[PhysicsEngine::PhysicsEngine] Exit");
}

PhysicsEngine::~PhysicsEngine() {
    Logger::Trace("[PhysicsEngine::~PhysicsEngine] Entry - destroying PhysicsEngine, body count=%zu, body index size=%zu",
                  m_bodies.size(), m_bodyIndex.size());
    Logger::Info("[PhysicsEngine::~PhysicsEngine] PhysicsEngine destroyed with %zu rigid bodies", m_bodies.size());
    Logger::Trace("[PhysicsEngine::~PhysicsEngine] Exit");
}

void PhysicsEngine::Initialize(const Vector3& gravity, float timeStep) {
    Logger::Trace("[PhysicsEngine::Initialize] Entry - gravity=(%.4f, %.4f, %.4f), timeStep=%.6f",
                  gravity.x, gravity.y, gravity.z, timeStep);
    m_gravity = gravity;
    m_timeStep = timeStep;
    Logger::Debug("[PhysicsEngine::Initialize] Stored gravity=(%.4f, %.4f, %.4f) and timeStep=%.6f",
                  m_gravity.x, m_gravity.y, m_gravity.z, m_timeStep);
    Logger::Info("PhysicsEngine initialized: gravity=(%.1f,%.1f,%.1f), dt=%.3f",
                 gravity.x, gravity.y, gravity.z, timeStep);
    Logger::Trace("[PhysicsEngine::Initialize] Exit");
}

uint32_t PhysicsEngine::AddRigidBody(const RigidBody& body) {
    Logger::Trace("[PhysicsEngine::AddRigidBody] Entry - body.id=%u, body.isStatic=%s, body.position=(%.4f, %.4f, %.4f), body.velocity=(%.4f, %.4f, %.4f)",
                  body.id, body.isStatic ? "true" : "false",
                  body.position.x, body.position.y, body.position.z,
                  body.velocity.x, body.velocity.y, body.velocity.z);
    uint32_t id = body.id;
    size_t newIndex = m_bodies.size();
    m_bodyIndex[id] = newIndex;
    m_bodies.push_back(body);
    Logger::Debug("[PhysicsEngine::AddRigidBody] Body %u inserted at index %zu, total bodies now=%zu",
                  id, newIndex, m_bodies.size());
    Logger::Info("[PhysicsEngine::AddRigidBody] Added rigid body id=%u (static=%s) at position (%.4f, %.4f, %.4f), total bodies=%zu",
                 id, body.isStatic ? "true" : "false",
                 body.position.x, body.position.y, body.position.z, m_bodies.size());
    Logger::Trace("[PhysicsEngine::AddRigidBody] Exit - returning id=%u", id);
    return id;
}

void PhysicsEngine::RemoveRigidBody(uint32_t bodyId) {
    Logger::Trace("[PhysicsEngine::RemoveRigidBody] Entry - bodyId=%u", bodyId);
    auto it = m_bodyIndex.find(bodyId);
    if (it == m_bodyIndex.end()) {
        Logger::Warn("[PhysicsEngine::RemoveRigidBody] Body id=%u not found in body index, nothing to remove", bodyId);
        Logger::Trace("[PhysicsEngine::RemoveRigidBody] Exit - body not found, early return");
        return;
    }
    size_t idx = it->second, last = m_bodies.size()-1;
    Logger::Debug("[PhysicsEngine::RemoveRigidBody] Body %u found at index %zu, last index=%zu, performing swap-and-pop removal",
                  bodyId, idx, last);
    if (idx != last) {
        Logger::Debug("[PhysicsEngine::RemoveRigidBody] Swapping body at index %zu (id=%u) with body at index %zu (id=%u)",
                      idx, m_bodies[idx].id, last, m_bodies[last].id);
    } else {
        Logger::Debug("[PhysicsEngine::RemoveRigidBody] Body %u is already at last index %zu, no swap needed", bodyId, idx);
    }
    std::swap(m_bodies[idx], m_bodies[last]);
    m_bodyIndex[m_bodies[idx].id] = idx;
    m_bodies.pop_back();
    m_bodyIndex.erase(bodyId);
    Logger::Info("[PhysicsEngine::RemoveRigidBody] Removed rigid body id=%u, total bodies now=%zu", bodyId, m_bodies.size());
    Logger::Trace("[PhysicsEngine::RemoveRigidBody] Exit");
}

void PhysicsEngine::Update() {
    Logger::Trace("[PhysicsEngine::Update] Entry - body count=%zu, gravity=(%.4f, %.4f, %.4f), timeStep=%.6f",
                  m_bodies.size(), m_gravity.x, m_gravity.y, m_gravity.z, m_timeStep);
    int integratedCount = 0;
    int staticCount = 0;
    for (auto& body : m_bodies) {
        if (!body.isStatic) {
            Logger::Debug("[PhysicsEngine::Update] Integrating dynamic body id=%u at position (%.4f, %.4f, %.4f) with velocity (%.4f, %.4f, %.4f)",
                          body.id, body.position.x, body.position.y, body.position.z,
                          body.velocity.x, body.velocity.y, body.velocity.z);
            Integrate(body);
            integratedCount++;
        } else {
            Logger::Debug("[PhysicsEngine::Update] Skipping static body id=%u at position (%.4f, %.4f, %.4f)",
                          body.id, body.position.x, body.position.y, body.position.z);
            staticCount++;
        }
    }
    Logger::Debug("[PhysicsEngine::Update] Integration phase complete: %d dynamic bodies integrated, %d static bodies skipped",
                  integratedCount, staticCount);
    Logger::Debug("[PhysicsEngine::Update] Starting collision detection phase");
    DetectCollisions();
    Logger::Info("[PhysicsEngine::Update] Physics update complete: integrated %d bodies, %d static, collision detection performed",
                 integratedCount, staticCount);
    Logger::Trace("[PhysicsEngine::Update] Exit");
}

void PhysicsEngine::Integrate(RigidBody& body) {
    Logger::Trace("[PhysicsEngine::Integrate] Entry - body.id=%u, position=(%.4f, %.4f, %.4f), velocity=(%.4f, %.4f, %.4f), acceleration=(%.4f, %.4f, %.4f)",
                  body.id, body.position.x, body.position.y, body.position.z,
                  body.velocity.x, body.velocity.y, body.velocity.z,
                  body.acceleration.x, body.acceleration.y, body.acceleration.z);
    body.acceleration = m_gravity;
    Logger::Debug("[PhysicsEngine::Integrate] Body %u: acceleration set to gravity=(%.4f, %.4f, %.4f)",
                  body.id, body.acceleration.x, body.acceleration.y, body.acceleration.z);
    Vector3 oldVelocity = body.velocity;
    body.velocity += body.acceleration * m_timeStep;
    Logger::Debug("[PhysicsEngine::Integrate] Body %u: velocity updated from (%.4f, %.4f, %.4f) to (%.4f, %.4f, %.4f) [delta=(%.4f, %.4f, %.4f), timeStep=%.6f]",
                  body.id,
                  oldVelocity.x, oldVelocity.y, oldVelocity.z,
                  body.velocity.x, body.velocity.y, body.velocity.z,
                  (body.velocity.x - oldVelocity.x), (body.velocity.y - oldVelocity.y), (body.velocity.z - oldVelocity.z),
                  m_timeStep);
    Vector3 oldPosition = body.position;
    body.position += body.velocity * m_timeStep;
    Logger::Debug("[PhysicsEngine::Integrate] Body %u: position updated from (%.4f, %.4f, %.4f) to (%.4f, %.4f, %.4f) [delta=(%.4f, %.4f, %.4f)]",
                  body.id,
                  oldPosition.x, oldPosition.y, oldPosition.z,
                  body.position.x, body.position.y, body.position.z,
                  (body.position.x - oldPosition.x), (body.position.y - oldPosition.y), (body.position.z - oldPosition.z));
    Logger::Trace("[PhysicsEngine::Integrate] Exit - body.id=%u final state: pos=(%.4f, %.4f, %.4f), vel=(%.4f, %.4f, %.4f), accel=(%.4f, %.4f, %.4f)",
                  body.id, body.position.x, body.position.y, body.position.z,
                  body.velocity.x, body.velocity.y, body.velocity.z,
                  body.acceleration.x, body.acceleration.y, body.acceleration.z);
}

void PhysicsEngine::DetectCollisions() {
    Logger::Trace("[PhysicsEngine::DetectCollisions] Entry - body count=%zu, collision callback %s",
                  m_bodies.size(), m_collisionCb ? "is set" : "is NOT set");
    int pairsChecked = 0;
    int collisionsFound = 0;
    for (size_t i=0; i<m_bodies.size(); ++i) {
        for (size_t j=i+1; j<m_bodies.size(); ++j) {
            auto& A = m_bodies[i], &B = m_bodies[j];
            float dist2 = (A.position - B.position).LengthSquared();
            float minDist = 1.0f; // assume unit radii
            pairsChecked++;
            Logger::Debug("[PhysicsEngine::DetectCollisions] Checking pair (id=%u, id=%u): dist2=%.4f, minDist2=%.4f",
                          A.id, B.id, dist2, minDist*minDist);
            if (dist2 < minDist*minDist) {
                collisionsFound++;
                Logger::Info("[PhysicsEngine::DetectCollisions] Collision detected between body %u at (%.4f, %.4f, %.4f) and body %u at (%.4f, %.4f, %.4f), dist2=%.4f < threshold=%.4f",
                             A.id, A.position.x, A.position.y, A.position.z,
                             B.id, B.position.x, B.position.y, B.position.z,
                             dist2, minDist*minDist);
                if (m_collisionCb) {
                    Logger::Debug("[PhysicsEngine::DetectCollisions] Invoking collision callback for pair (%u, %u)", A.id, B.id);
                    m_collisionCb(A.id, B.id);
                } else {
                    Logger::Debug("[PhysicsEngine::DetectCollisions] No collision callback set, skipping notification for pair (%u, %u)", A.id, B.id);
                }
            }
        }
    }
    Logger::Debug("[PhysicsEngine::DetectCollisions] Detection complete: %d pairs checked, %d collisions found", pairsChecked, collisionsFound);
    Logger::Trace("[PhysicsEngine::DetectCollisions] Exit - pairsChecked=%d, collisionsFound=%d", pairsChecked, collisionsFound);
}

bool PhysicsEngine::Raycast(const Vector3& origin,
                            const Vector3& direction,
                            float maxDistance,
                            uint32_t& outBodyId,
                            Vector3& outHitPoint)
{
    Logger::Trace("[PhysicsEngine::Raycast] Entry - origin=(%.4f, %.4f, %.4f), direction=(%.4f, %.4f, %.4f), maxDistance=%.4f, body count=%zu",
                  origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, maxDistance, m_bodies.size());
    // Simple sphere intersection test
    int bodiesTested = 0;
    for (auto& body : m_bodies) {
        bodiesTested++;
        Logger::Debug("[PhysicsEngine::Raycast] Testing body id=%u at position (%.4f, %.4f, %.4f)",
                      body.id, body.position.x, body.position.y, body.position.z);
        Vector3 L = body.position - origin;
        float tca = L.Dot(direction);
        Logger::Debug("[PhysicsEngine::Raycast] Body %u: L=(%.4f, %.4f, %.4f), tca=%.4f", body.id, L.x, L.y, L.z, tca);
        if (tca < 0) {
            Logger::Debug("[PhysicsEngine::Raycast] Body %u: tca=%.4f < 0, body is behind ray origin, skipping", body.id, tca);
            continue;
        }
        float d2 = L.Dot(L) - tca*tca;
        float radius = 1.0f;
        Logger::Debug("[PhysicsEngine::Raycast] Body %u: d2=%.4f, radius2=%.4f", body.id, d2, radius*radius);
        if (d2 > radius*radius) {
            Logger::Debug("[PhysicsEngine::Raycast] Body %u: d2=%.4f > radius2=%.4f, ray misses sphere, skipping", body.id, d2, radius*radius);
            continue;
        }
        float thc = sqrt(radius*radius - d2);
        float t = tca - thc;
        Logger::Debug("[PhysicsEngine::Raycast] Body %u: thc=%.4f, initial t=%.4f (tca - thc)", body.id, thc, t);
        if (t < 0) {
            t = tca + thc;
            Logger::Debug("[PhysicsEngine::Raycast] Body %u: t was negative, using far intersection t=%.4f (tca + thc)", body.id, t);
        }
        if (t > maxDistance) {
            Logger::Debug("[PhysicsEngine::Raycast] Body %u: t=%.4f > maxDistance=%.4f, hit too far, skipping", body.id, t, maxDistance);
            continue;
        }
        outBodyId = body.id;
        outHitPoint = origin + direction * t;
        Logger::Info("[PhysicsEngine::Raycast] Ray HIT body %u at t=%.4f, hitPoint=(%.4f, %.4f, %.4f), bodies tested=%d",
                     outBodyId, t, outHitPoint.x, outHitPoint.y, outHitPoint.z, bodiesTested);
        Logger::Trace("[PhysicsEngine::Raycast] Exit - returning true, outBodyId=%u, outHitPoint=(%.4f, %.4f, %.4f)",
                      outBodyId, outHitPoint.x, outHitPoint.y, outHitPoint.z);
        return true;
    }
    Logger::Debug("[PhysicsEngine::Raycast] No hit found after testing %d bodies", bodiesTested);
    Logger::Trace("[PhysicsEngine::Raycast] Exit - returning false, no intersection found");
    return false;
}

void PhysicsEngine::SetCollisionCallback(CollisionCallback cb) {
    Logger::Trace("[PhysicsEngine::SetCollisionCallback] Entry - callback %s", cb ? "is valid" : "is null");
    m_collisionCb = std::move(cb);
    Logger::Info("[PhysicsEngine::SetCollisionCallback] Collision callback %s", m_collisionCb ? "set successfully" : "cleared (null)");
    Logger::Trace("[PhysicsEngine::SetCollisionCallback] Exit");
}

void PhysicsEngine::TriggerParticle(const ParticleEffect& effect, const Vector3& position) {
    Logger::Trace("[PhysicsEngine::TriggerParticle] Entry - effect.name='%s', position=(%.4f, %.4f, %.4f)",
                  effect.name.c_str(), position.x, position.y, position.z);
    // Forward to game world or particle system
    Logger::Info("Triggering particle '%s' at (%.1f,%.1f,%.1f)",
                 effect.name.c_str(), position.x, position.y, position.z);
    Logger::Debug("[PhysicsEngine::TriggerParticle] Particle effect '%s' forwarded to particle system at position (%.4f, %.4f, %.4f)",
                  effect.name.c_str(), position.x, position.y, position.z);
    Logger::Trace("[PhysicsEngine::TriggerParticle] Exit");
}
