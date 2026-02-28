// src/Game/ProjectileManager.cpp
// RS2V projectile system — manages projectile lifecycle, physics simulation, and hit detection

#include "Game/ProjectileManager.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/WeaponDatabase.h"
#include "Network/NetworkManager.h"
#include "Network/Packet.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <random>

// Player collision radius for hit detection (meters)
static constexpr float PLAYER_COLLISION_RADIUS = 0.5f;
// Player height for hit zone calculations (meters)
static constexpr float PLAYER_HEIGHT = 1.8f;

ProjectileManager::ProjectileManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ProjectileManager::ProjectileManager] Entry: server=%p", server);
    Logger::Trace("[ProjectileManager::ProjectileManager] Exit");
}

ProjectileManager::~ProjectileManager() {
    Logger::Trace("[ProjectileManager::~ProjectileManager] Entry");
    Shutdown();
    Logger::Trace("[ProjectileManager::~ProjectileManager] Exit");
}

void ProjectileManager::Initialize() {
    Logger::Trace("[ProjectileManager::Initialize] Entry");
    m_projectiles.clear();
    m_nextProjectileId = 1;
    Logger::Info("ProjectileManager initialized");
    Logger::Trace("[ProjectileManager::Initialize] Exit");
}

void ProjectileManager::Shutdown() {
    Logger::Trace("[ProjectileManager::Shutdown] Entry");
    Logger::Debug("[ProjectileManager::Shutdown] Clearing %zu active projectiles", m_projectiles.size());
    m_projectiles.clear();
    Logger::Trace("[ProjectileManager::Shutdown] Exit");
}

uint32_t ProjectileManager::SpawnProjectile(uint32_t shooterId, const std::string& weaponId,
                                             const Vector3& origin, const Vector3& direction,
                                             float speed, float damage, float gravity,
                                             float explosionRadius, float maxExplosionDamage,
                                             DamageSource source) {
    Logger::Trace("[ProjectileManager::SpawnProjectile] Entry: shooterId=%u, weaponId='%s'", shooterId, weaponId.c_str());

    Projectile proj;
    proj.id = m_nextProjectileId++;
    proj.shooterId = shooterId;
    proj.weaponId = weaponId;
    proj.origin = origin;
    proj.position = origin;
    proj.direction = direction.Normalized();
    proj.velocity = proj.direction * speed;
    proj.speed = speed;
    proj.gravity = gravity;
    proj.damage = damage;
    proj.explosionRadius = explosionRadius;
    proj.maxExplosionDamage = maxExplosionDamage;
    proj.damageSource = source;
    proj.lifetime = 0.0f;
    proj.maxLifetime = 10.0f;
    proj.active = true;

    m_projectiles.push_back(proj);

    Logger::Info("[ProjectileManager::SpawnProjectile] Projectile %u spawned: weapon='%s', speed=%.1f, pos=(%.1f,%.1f,%.1f)",
                 proj.id, weaponId.c_str(), speed, origin.x, origin.y, origin.z);
    Logger::Trace("[ProjectileManager::SpawnProjectile] Exit: returning id=%u", proj.id);
    return proj.id;
}

HitscanResult ProjectileManager::PerformHitscan(uint32_t shooterId, const Vector3& origin,
                                                  const Vector3& direction, float maxRange,
                                                  float accuracy) {
    Logger::Trace("[ProjectileManager::PerformHitscan] Entry: shooterId=%u, maxRange=%.1f, accuracy=%.2f",
                  shooterId, maxRange, accuracy);

    HitscanResult result;
    result.hit = false;

    Vector3 dir = direction.Normalized();

    // Apply accuracy spread
    if (accuracy < 1.0f) {
        static thread_local std::mt19937 rng(std::random_device{}());
        float spread = (1.0f - accuracy) * 0.05f; // max ~2.9 degrees at 0 accuracy
        std::normal_distribution<float> dist(0.0f, spread);
        dir.x += dist(rng);
        dir.y += dist(rng);
        dir.z += dist(rng);
        dir.Normalize();
    }

    auto* pm = m_server->GetPlayerManager();
    auto alivePlayers = pm->GetAlivePlayers();

    float closestDist = maxRange;
    uint32_t closestVictim = 0;
    Vector3 closestHitPos;

    for (auto& player : alivePlayers) {
        uint32_t pid = player->GetConnection()->GetClientId();
        if (pid == shooterId) continue; // Can't shoot yourself

        Vector3 playerPos = player->GetPosition();

        // Ray-sphere intersection test
        // Sphere center at player position (elevated to center mass), radius = PLAYER_COLLISION_RADIUS
        Vector3 sphereCenter = playerPos;
        sphereCenter.z += PLAYER_HEIGHT * 0.5f; // Center mass

        Vector3 oc = origin - sphereCenter;
        float a = dir.Dot(dir);
        float b = 2.0f * oc.Dot(dir);
        float c = oc.Dot(oc) - PLAYER_COLLISION_RADIUS * PLAYER_COLLISION_RADIUS;
        float discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0.0f) continue; // No intersection

        float sqrtDisc = std::sqrt(discriminant);
        float t = (-b - sqrtDisc) / (2.0f * a);
        if (t < 0.0f) t = (-b + sqrtDisc) / (2.0f * a); // Try other root
        if (t < 0.0f || t > closestDist) continue; // Behind us or farther than current closest

        closestDist = t;
        closestVictim = pid;
        closestHitPos = origin + dir * t;
    }

    if (closestVictim != 0) {
        result.hit = true;
        result.victimId = closestVictim;
        result.hitPosition = closestHitPos;
        result.hitNormal = (closestHitPos - origin).Normalized() * -1.0f;
        result.distance = closestDist;

        auto victim = pm->GetPlayer(closestVictim);
        if (victim) {
            result.hitZone = DetermineHitZone(dir, victim->GetPosition(), closestHitPos);
        }

        Logger::Debug("[ProjectileManager::PerformHitscan] Hit player %u at dist=%.1f, zone=%d",
                      closestVictim, closestDist, static_cast<int>(result.hitZone));
    } else {
        Logger::Debug("[ProjectileManager::PerformHitscan] Miss (no player hit within range %.1f)", maxRange);
    }

    Logger::Trace("[ProjectileManager::PerformHitscan] Exit: hit=%d", result.hit);
    return result;
}

void ProjectileManager::Update(float deltaSeconds) {
    // Hot loop - minimal logging
    for (auto& proj : m_projectiles) {
        if (!proj.active) continue;

        proj.lifetime += deltaSeconds;
        if (proj.lifetime >= proj.maxLifetime) {
            Logger::Debug("[ProjectileManager::Update] Projectile %u expired (lifetime=%.1fs)", proj.id, proj.lifetime);
            proj.active = false;
            continue;
        }

        // Apply gravity to velocity
        proj.velocity.z -= proj.gravity * deltaSeconds;

        // Move projectile
        Vector3 prevPos = proj.position;
        proj.position += proj.velocity * deltaSeconds;

        // Check for ground collision (z <= 0)
        if (proj.position.z <= 0.0f) {
            proj.position.z = 0.0f;
            Logger::Debug("[ProjectileManager::Update] Projectile %u hit ground at (%.1f,%.1f,%.1f)",
                          proj.id, proj.position.x, proj.position.y, proj.position.z);
            if (proj.explosionRadius > 0.0f) {
                DetonateProjectile(proj);
            }
            proj.active = false;
            continue;
        }

        // Check for player collision
        if (CheckProjectileCollision(proj, deltaSeconds)) {
            proj.active = false;
        }
    }

    // Remove inactive projectiles
    m_projectiles.erase(
        std::remove_if(m_projectiles.begin(), m_projectiles.end(),
                        [](const Projectile& p) { return !p.active; }),
        m_projectiles.end());
}

bool ProjectileManager::CheckProjectileCollision(Projectile& proj, float deltaSeconds) {
    auto* pm = m_server->GetPlayerManager();
    auto alivePlayers = pm->GetAlivePlayers();

    for (auto& player : alivePlayers) {
        uint32_t pid = player->GetConnection()->GetClientId();
        if (pid == proj.shooterId) continue;

        Vector3 playerPos = player->GetPosition();
        playerPos.z += PLAYER_HEIGHT * 0.5f; // Center mass

        float dist = proj.position.Distance(playerPos);
        if (dist <= PLAYER_COLLISION_RADIUS + 0.5f) { // Projectile radius ~0.5m
            Logger::Debug("[ProjectileManager::CheckProjectileCollision] Projectile %u hit player %u at dist=%.2f",
                          proj.id, pid, dist);

            if (proj.explosionRadius > 0.0f) {
                // Explosive projectile - detonate on contact
                DetonateProjectile(proj);
            } else {
                // Direct hit - apply damage
                auto* ds = m_server->GetDamageSystem();
                if (ds) {
                    HitZone zone = DetermineHitZone(proj.direction, player->GetPosition(), proj.position);
                    DamageEvent event;
                    event.attackerId = proj.shooterId;
                    event.victimId = pid;
                    event.source = proj.damageSource;
                    event.weaponId = proj.weaponId;
                    event.hitZone = zone;
                    event.baseDamage = proj.damage;
                    event.distance = proj.origin.Distance(proj.position);
                    event.hitPosition = proj.position;
                    event.hitDirection = proj.direction;
                    ds->ApplyDamage(event);
                }
            }
            return true;
        }
    }
    return false;
}

void ProjectileManager::DetonateProjectile(const Projectile& proj) {
    Logger::Info("[ProjectileManager::DetonateProjectile] Projectile %u detonating at (%.1f,%.1f,%.1f), radius=%.1f",
                 proj.id, proj.position.x, proj.position.y, proj.position.z, proj.explosionRadius);

    auto* ds = m_server->GetDamageSystem();
    if (ds) {
        ds->ApplyExplosionDamage(proj.shooterId, proj.weaponId,
                                  proj.position, proj.explosionRadius,
                                  proj.maxExplosionDamage, proj.damageSource);
    }

    // Broadcast explosion effect to clients
    auto* nm = m_server->GetNetworkManager();
    if (nm) {
        Packet pkt("EXPLOSION_EFFECT");
        pkt.WriteVector3(proj.position);
        pkt.WriteFloat(proj.explosionRadius);
        pkt.WriteString(proj.weaponId);
        nm->BroadcastPacket(pkt);
    }
}

void ProjectileManager::ApplyHitscanDamage(uint32_t shooterId, const std::string& weaponId,
                                            const HitscanResult& result) {
    if (!result.hit) return;

    Logger::Debug("[ProjectileManager::ApplyHitscanDamage] shooterId=%u, victimId=%u, weapon='%s', zone=%d, dist=%.1f",
                  shooterId, result.victimId, weaponId.c_str(), static_cast<int>(result.hitZone), result.distance);

    auto* ds = m_server->GetDamageSystem();
    auto* wdb = m_server->GetWeaponDatabase();
    if (!ds) return;

    float baseDmg = 50.0f; // Default
    if (wdb) {
        bool isHeadshot = (result.hitZone == HitZone::Head);
        bool isLimb = (result.hitZone == HitZone::LeftArm || result.hitZone == HitZone::RightArm ||
                       result.hitZone == HitZone::LeftLeg || result.hitZone == HitZone::RightLeg);
        baseDmg = wdb->CalculateDamage(weaponId, result.distance, isHeadshot, isLimb);
    }

    DamageEvent event;
    event.attackerId = shooterId;
    event.victimId = result.victimId;
    event.source = DamageSource::Bullet;
    event.weaponId = weaponId;
    event.hitZone = result.hitZone;
    event.baseDamage = baseDmg;
    event.distance = result.distance;
    event.hitPosition = result.hitPosition;
    event.hitDirection = result.hitNormal * -1.0f;
    ds->ApplyDamage(event);
}

HitZone ProjectileManager::DetermineHitZone(const Vector3& hitDirection, const Vector3& playerPos,
                                              const Vector3& hitPos) const {
    // Calculate hit height relative to player feet
    float relativeHeight = hitPos.z - playerPos.z;
    float heightRatio = relativeHeight / PLAYER_HEIGHT;

    if (heightRatio > 0.85f) {
        return HitZone::Head;
    } else if (heightRatio > 0.55f) {
        return HitZone::Chest;
    } else if (heightRatio > 0.40f) {
        return HitZone::Stomach;
    } else {
        // Below waist - determine arm vs leg based on horizontal offset
        Vector3 horizontal = hitPos - playerPos;
        horizontal.z = 0.0f;
        float lateralOffset = std::abs(horizontal.x);

        if (lateralOffset > 0.2f) {
            // Outer hit - arm or leg
            if (heightRatio > 0.25f) {
                return (hitPos.x > playerPos.x) ? HitZone::RightArm : HitZone::LeftArm;
            } else {
                return (hitPos.x > playerPos.x) ? HitZone::RightLeg : HitZone::LeftLeg;
            }
        }
        return (heightRatio > 0.25f) ? HitZone::Stomach : HitZone::LeftLeg;
    }
}

void ProjectileManager::BroadcastFireEvent(uint32_t shooterId, const std::string& weaponId,
                                            const Vector3& origin, const Vector3& direction) {
    Logger::Trace("[ProjectileManager::BroadcastFireEvent] Entry: shooterId=%u, weaponId='%s'", shooterId, weaponId.c_str());

    auto* nm = m_server->GetNetworkManager();
    if (!nm) {
        Logger::Debug("[ProjectileManager::BroadcastFireEvent] No NetworkManager available");
        return;
    }

    Packet pkt("WEAPON_FIRE_EFFECT");
    pkt.WriteUInt(shooterId);
    pkt.WriteString(weaponId);
    pkt.WriteVector3(origin);
    pkt.WriteVector3(direction);
    nm->BroadcastPacket(pkt);

    Logger::Debug("[ProjectileManager::BroadcastFireEvent] Fire event broadcast for player %u weapon '%s'",
                  shooterId, weaponId.c_str());
    Logger::Trace("[ProjectileManager::BroadcastFireEvent] Exit");
}

const std::vector<Projectile>& ProjectileManager::GetActiveProjectiles() const {
    return m_projectiles;
}

size_t ProjectileManager::GetActiveProjectileCount() const {
    return m_projectiles.size();
}
