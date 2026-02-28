// src/Game/Weapon.cpp – Implementation for Weapon

#include "Game/Weapon.h"
#include "Utils/Logger.h"
#include "Game/Player.h"
#include "Game/GameServer.h"
#include "Game/ProjectileManager.h"
#include "Game/WeaponDatabase.h"
#include "Network/NetworkManager.h"
#include <algorithm>

Weapon::Weapon(const std::string& name, const WeaponStats& stats,
               const std::string& weaponId, GameServer* server)
    : m_name(name)
    , m_weaponId(weaponId)
    , m_stats(stats)
    , m_fireMode(FireMode::SemiAuto)
    , m_server(server)
    , m_ammoInMagazine(stats.magazineSize)
    , m_remainingAmmo(stats.maxAmmo - stats.magazineSize)
    , m_timeSinceLastShot(0.0f)
    , m_isReloading(false)
    , m_reloadTimer(0.0f)
{
    Logger::Info("Weapon '%s' created: DAMAGE=%.1f, RANGE=%.1f, FIRE_RATE=%.1f RPM",
                 m_name.c_str(), stats.damage, stats.range, stats.fireRate);
}

Weapon::~Weapon() = default;

float Weapon::ShotsPerSecond() const {
    return m_stats.fireRate / 60.0f;
}

bool Weapon::CanFire() const {
    if (m_isReloading) return false;
    if (m_ammoInMagazine <= 0) return false;
    return m_timeSinceLastShot >= (1.0f / ShotsPerSecond());
}

void Weapon::Fire(const Vector3& origin, const Vector3& direction, Player* shooter) {
    if (!CanFire()) {
        if (m_ammoInMagazine == 0) Reload();
        return;
    }
    // Spawn projectile or hitscan
    SpawnProjectile(origin, direction, shooter);

    m_ammoInMagazine--;
    m_timeSinceLastShot = 0.0f;

    Logger::Debug("Weapon '%s' fired by player %u, ammo left: %d/%d",
                  m_name.c_str(),
                  shooter ? shooter->GetConnection()->GetClientId() : 0,
                  m_ammoInMagazine, m_remainingAmmo);

    if (m_ammoInMagazine == 0) {
        Reload();
    }
}

void Weapon::UpdateFireTimer(float deltaSeconds) {
    m_timeSinceLastShot += deltaSeconds;
}

bool Weapon::NeedsReload() const {
    return m_ammoInMagazine < m_stats.magazineSize && m_remainingAmmo > 0;
}

void Weapon::Reload() {
    if (m_isReloading || m_remainingAmmo == 0 || m_ammoInMagazine == m_stats.magazineSize)
        return;

    m_isReloading = true;
    m_reloadTimer = 0.0f;
    Logger::Info("Reloading '%s'", m_name.c_str());
}

void Weapon::UpdateReloadTimer(float deltaSeconds) {
    if (!m_isReloading) return;
    m_reloadTimer += deltaSeconds;
    if (m_reloadTimer >= m_stats.reloadTime) {
        int needed = m_stats.magazineSize - m_ammoInMagazine;
        int toLoad = std::min(needed, m_remainingAmmo);
        m_ammoInMagazine += toLoad;
        m_remainingAmmo -= toLoad;
        m_isReloading = false;
        Logger::Info("Reload complete '%s', ammo: %d/%d",
                     m_name.c_str(), m_ammoInMagazine, m_remainingAmmo);
    }
}

int Weapon::GetAmmoInMagazine() const {
    return m_ammoInMagazine;
}

int Weapon::GetRemainingAmmo() const {
    return m_remainingAmmo;
}

bool Weapon::AddAmmo(int amount) {
    if (amount <= 0) return false;
    m_remainingAmmo += amount;
    m_remainingAmmo = std::min(m_remainingAmmo, m_stats.maxAmmo);
    Logger::Info("Added %d ammo to '%s', now %d/%d",
                 amount, m_name.c_str(), m_ammoInMagazine, m_remainingAmmo);
    return true;
}

const std::string& Weapon::GetName() const {
    return m_name;
}

const WeaponStats& Weapon::GetStats() const {
    return m_stats;
}

FireMode Weapon::GetFireMode() const {
    return m_fireMode;
}

void Weapon::SetFireMode(FireMode mode) {
    m_fireMode = mode;
}

void Weapon::SpawnProjectile(const Vector3& origin, const Vector3& direction, Player* shooter) {
    if (!shooter) return;

    uint32_t shooterId = shooter->GetConnection() ? shooter->GetConnection()->GetClientId() : 0;
    std::string weaponId = m_weaponId.empty() ? m_name : m_weaponId;

    // Look up weapon definition for ballistic properties
    const WeaponDefinition* weaponDef = nullptr;
    if (m_server) {
        auto* wdb = m_server->GetWeaponDatabase();
        if (wdb) {
            weaponDef = wdb->GetWeapon(weaponId);
        }
    }

    // Broadcast fire event to all clients for visual/audio effects
    if (m_server) {
        auto* projMgr = m_server->GetProjectileManager();
        if (projMgr) {
            projMgr->BroadcastFireEvent(shooterId, weaponId, origin, direction);
        }
    }

    // Determine if this is a physical projectile or hitscan
    bool isPhysicalProjectile = false;
    float projectileSpeed = 0.0f;
    float explosionRadius = 0.0f;
    float maxExplosionDamage = 0.0f;
    float gravity = 9.81f;
    DamageSource damageSource = DamageSource::Bullet;

    if (weaponDef) {
        isPhysicalProjectile = weaponDef->ballistics.isProjectile;
        projectileSpeed = weaponDef->ballistics.projectileSpeed;
        gravity = weaponDef->ballistics.bulletDrop;

        // Determine explosion properties from weapon category
        if (weaponDef->category == WeaponCategory::RocketLauncher ||
            weaponDef->category == WeaponCategory::GrenadeLauncher) {
            explosionRadius = weaponDef->ballistics.suppressionRadius;
            maxExplosionDamage = weaponDef->stats.damage;
            damageSource = DamageSource::Explosion;
        }
        if (weaponDef->category == WeaponCategory::Flamethrower) {
            damageSource = DamageSource::Fire;
        }
    }

    if (isPhysicalProjectile && m_server) {
        // Spawn a physical projectile (rocket, grenade, etc.)
        auto* projMgr = m_server->GetProjectileManager();
        if (projMgr) {
            projMgr->SpawnProjectile(shooterId, weaponId, origin, direction,
                                      projectileSpeed, m_stats.damage, gravity,
                                      explosionRadius, maxExplosionDamage, damageSource);
            Logger::Debug("[Weapon::SpawnProjectile] Physical projectile spawned for '%s' by player %u",
                          weaponId.c_str(), shooterId);
        }
    } else if (m_server) {
        // Hitscan weapon - perform instant ray trace
        auto* projMgr = m_server->GetProjectileManager();
        if (projMgr) {
            HitscanResult result = projMgr->PerformHitscan(shooterId, origin, direction,
                                                            m_stats.range, m_stats.accuracy);
            if (result.hit) {
                // Apply damage through the DamageSystem
                auto* ds = m_server->GetDamageSystem();
                auto* wdb = m_server->GetWeaponDatabase();
                if (ds) {
                    float baseDmg = m_stats.damage;
                    if (wdb) {
                        bool isHeadshot = (result.hitZone == HitZone::Head);
                        bool isLimb = (result.hitZone == HitZone::LeftArm || result.hitZone == HitZone::RightArm ||
                                       result.hitZone == HitZone::LeftLeg || result.hitZone == HitZone::RightLeg);
                        baseDmg = wdb->CalculateDamage(weaponId, result.distance, isHeadshot, isLimb);
                    }

                    DamageEvent event;
                    event.attackerId = shooterId;
                    event.victimId = result.victimId;
                    event.source = damageSource;
                    event.weaponId = weaponId;
                    event.hitZone = result.hitZone;
                    event.baseDamage = baseDmg;
                    event.distance = result.distance;
                    event.hitPosition = result.hitPosition;
                    event.hitDirection = direction.Normalized();
                    ds->ApplyDamage(event);
                }
                Logger::Debug("[Weapon::SpawnProjectile] Hitscan hit player %u at dist=%.1f, zone=%d",
                              result.victimId, result.distance, static_cast<int>(result.hitZone));
            } else {
                Logger::Debug("[Weapon::SpawnProjectile] Hitscan miss by player %u with '%s'",
                              shooterId, weaponId.c_str());
            }
        }
    } else {
        // No server context (standalone weapon) - log only
        Logger::Debug("[Weapon::SpawnProjectile] No server context for '%s', fire event logged only", m_name.c_str());
    }
}