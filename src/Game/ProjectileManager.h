// src/Game/ProjectileManager.h
// RS2V projectile system — manages projectile lifecycle, physics simulation, and hit detection

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "Math/Vector3.h"
#include "Game/DamageSystem.h"

class GameServer;
class Player;

// Represents an active projectile in the world
struct Projectile {
    uint32_t id = 0;
    uint32_t shooterId = 0;
    std::string weaponId;
    Vector3 position;
    Vector3 velocity;
    Vector3 origin;
    Vector3 direction;
    float gravity = 9.81f;
    float speed = 0.0f;
    float lifetime = 0.0f;
    float maxLifetime = 10.0f;       // seconds before despawn
    float damage = 0.0f;
    float explosionRadius = 0.0f;    // > 0 means explosive projectile
    float maxExplosionDamage = 0.0f;
    DamageSource damageSource = DamageSource::Bullet;
    bool active = true;
};

// Result of a hitscan ray trace
struct HitscanResult {
    bool hit = false;
    uint32_t victimId = 0;
    Vector3 hitPosition;
    Vector3 hitNormal;
    HitZone hitZone = HitZone::Chest;
    float distance = 0.0f;
};

class ProjectileManager {
public:
    explicit ProjectileManager(GameServer* server);
    ~ProjectileManager();

    void Initialize();
    void Shutdown();

    // Spawn a physical projectile (rockets, grenades, etc.)
    uint32_t SpawnProjectile(uint32_t shooterId, const std::string& weaponId,
                             const Vector3& origin, const Vector3& direction,
                             float speed, float damage, float gravity,
                             float explosionRadius, float maxExplosionDamage,
                             DamageSource source);

    // Perform hitscan trace (bullets)
    HitscanResult PerformHitscan(uint32_t shooterId, const Vector3& origin,
                                  const Vector3& direction, float maxRange,
                                  float accuracy);

    // Per-tick update: move projectiles, check collisions, expire old projectiles
    void Update(float deltaSeconds);

    // Broadcast a weapon fire event to all clients
    void BroadcastFireEvent(uint32_t shooterId, const std::string& weaponId,
                            const Vector3& origin, const Vector3& direction);

    // Accessors
    const std::vector<Projectile>& GetActiveProjectiles() const;
    size_t GetActiveProjectileCount() const;

private:
    GameServer* m_server;
    std::vector<Projectile> m_projectiles;
    uint32_t m_nextProjectileId = 1;

    // Check if a projectile hits any player
    bool CheckProjectileCollision(Projectile& proj, float deltaSeconds);

    // Detonate an explosive projectile
    void DetonateProjectile(const Projectile& proj);

    // Apply hitscan damage
    void ApplyHitscanDamage(uint32_t shooterId, const std::string& weaponId,
                            const HitscanResult& result);

    // Determine hit zone from hit direction and position relative to player
    HitZone DetermineHitZone(const Vector3& hitDirection, const Vector3& playerPos,
                              const Vector3& hitPos) const;
};
