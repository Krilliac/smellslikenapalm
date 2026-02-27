// src/Game/DamageSystem.cpp
// RS2V damage processing — hit detection, damage calculation, kill tracking

#include "Game/DamageSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>

DamageSystem::DamageSystem(GameServer* server)
    : m_server(server)
{
}

DamageSystem::~DamageSystem() {
    Shutdown();
}

void DamageSystem::Initialize() {
    m_activeFireEffects.clear();
    Logger::Info("DamageSystem initialized");
}

void DamageSystem::Shutdown() {
    m_activeFireEffects.clear();
}

float DamageSystem::GetHitZoneMultiplier(HitZone zone) const {
    // RS2V damage model: head/spine = instant kill, chest = high, limbs = reduced
    switch (zone) {
        case HitZone::Head:      return 10.0f;  // Always lethal (100hp, any weapon)
        case HitZone::Chest:     return 1.0f;
        case HitZone::Stomach:   return 0.9f;
        case HitZone::LeftArm:
        case HitZone::RightArm:  return 0.5f;
        case HitZone::LeftLeg:
        case HitZone::RightLeg:  return 0.4f;
        default:                 return 1.0f;
    }
}

bool DamageSystem::IsFriendlyFire(uint32_t attackerId, uint32_t victimId) const {
    if (attackerId == 0 || attackerId == victimId) return false;
    auto* tm = m_server->GetTeamManager();
    return tm->GetPlayerTeam(attackerId) == tm->GetPlayerTeam(victimId);
}

void DamageSystem::ApplyDamage(const DamageEvent& event) {
    auto* pm = m_server->GetPlayerManager();
    auto victim = pm->GetPlayer(event.victimId);
    if (!victim || !victim->IsAlive()) return;

    DamageEvent e = event;
    e.isFriendlyFire = IsFriendlyFire(e.attackerId, e.victimId);
    e.isHeadshot = (e.hitZone == HitZone::Head);
    e.finalDamage = CalculateFinalDamage(e);

    // Apply damage to victim
    int currentHp = victim->GetHealth();
    int newHp = currentHp - static_cast<int>(std::ceil(e.finalDamage));
    if (newHp < 0) newHp = 0;
    victim->SetHealth(newHp);

    if (newHp <= 0) {
        e.isKill = true;
        ProcessKill(e);
    }

    if (m_damageCallback) m_damageCallback(e);
    BroadcastDamageEvent(e);

    Logger::Debug("Damage: %u -> %u, weapon=%s, zone=%d, base=%.1f, final=%.1f, hp=%d->%d%s",
                  e.attackerId, e.victimId, e.weaponId.c_str(), static_cast<int>(e.hitZone),
                  e.baseDamage, e.finalDamage, currentHp, newHp,
                  e.isKill ? " [KILL]" : "");
}

void DamageSystem::ApplyExplosionDamage(uint32_t attackerId, const std::string& weaponId,
                                         const Vector3& center, float radius,
                                         float maxDamage, DamageSource source) {
    auto* pm = m_server->GetPlayerManager();
    for (auto& player : pm->GetAlivePlayers()) {
        uint32_t pid = player->GetConnection()->GetClientId();
        Vector3 playerPos = player->GetPosition();
        float dist = playerPos.Distance(center);

        if (dist > radius) continue;

        // Quadratic falloff from center
        float falloff = 1.0f - (dist / radius);
        falloff *= falloff;  // Quadratic
        float dmg = maxDamage * falloff;

        DamageEvent e;
        e.attackerId = attackerId;
        e.victimId = pid;
        e.source = source;
        e.weaponId = weaponId;
        e.hitZone = HitZone::Chest;  // Explosions hit torso
        e.baseDamage = dmg;
        e.distance = dist;
        e.hitPosition = center;
        e.hitDirection = (playerPos - center).Normalized();

        ApplyDamage(e);
    }
}

void DamageSystem::ApplyFireDamage(uint32_t attackerId, const Vector3& center,
                                    float radius, float damagePerSecond, float duration) {
    FireEffect effect;
    effect.attackerId = attackerId;
    effect.center = center;
    effect.radius = radius;
    effect.damagePerSecond = damagePerSecond;
    effect.remaining = duration;
    effect.tickTimer = 0.0f;
    m_activeFireEffects.push_back(effect);

    Logger::Debug("Fire effect created at (%.1f, %.1f, %.1f), radius=%.1f, dps=%.1f, duration=%.1fs",
                  center.x, center.y, center.z, radius, damagePerSecond, duration);
}

void DamageSystem::SetOnDamage(DamageCallback cb) { m_damageCallback = std::move(cb); }
void DamageSystem::SetOnKill(KillCallback cb) { m_killCallback = std::move(cb); }

void DamageSystem::Update(float deltaSeconds) {
    // Process fire DoT effects
    for (auto it = m_activeFireEffects.begin(); it != m_activeFireEffects.end();) {
        it->remaining -= deltaSeconds;
        it->tickTimer += deltaSeconds;

        if (it->remaining <= 0.0f) {
            it = m_activeFireEffects.erase(it);
            continue;
        }

        // Tick fire damage every 0.5 seconds
        if (it->tickTimer >= 0.5f) {
            it->tickTimer -= 0.5f;
            float tickDmg = it->damagePerSecond * 0.5f;
            ApplyExplosionDamage(it->attackerId, "Fire", it->center,
                                 it->radius, tickDmg, DamageSource::Fire);
        }
        ++it;
    }
}

void DamageSystem::SetFriendlyFireEnabled(bool enabled) { m_friendlyFireEnabled = enabled; }
void DamageSystem::SetFriendlyFireDamageScale(float scale) { m_friendlyFireScale = scale; }

float DamageSystem::CalculateFinalDamage(DamageEvent& event) const {
    float dmg = event.baseDamage;

    // Apply hit zone multiplier
    dmg *= GetHitZoneMultiplier(event.hitZone);

    // Apply friendly fire scaling
    if (event.isFriendlyFire) {
        if (!m_friendlyFireEnabled) return 0.0f;
        dmg *= m_friendlyFireScale;
    }

    return std::max(0.0f, dmg);
}

void DamageSystem::ProcessKill(const DamageEvent& event) {
    KillEvent kill;
    kill.killerId = event.attackerId;
    kill.victimId = event.victimId;
    kill.weaponId = event.weaponId;
    kill.source = event.source;
    kill.distance = event.distance;
    kill.isHeadshot = event.isHeadshot;
    kill.isFriendlyFire = event.isFriendlyFire;
    kill.isTeamKill = event.isFriendlyFire;

    // Notify player manager
    auto* pm = m_server->GetPlayerManager();
    pm->OnPlayerDeath(event.victimId);

    if (m_killCallback) m_killCallback(kill);
    BroadcastKillEvent(kill);

    Logger::Info("Kill: %u killed %u with %s (dist=%.1fm%s%s)",
                 kill.killerId, kill.victimId, kill.weaponId.c_str(), kill.distance,
                 kill.isHeadshot ? ", headshot" : "",
                 kill.isTeamKill ? ", TEAMKILL" : "");
}

void DamageSystem::BroadcastDamageEvent(const DamageEvent& /*event*/) const {
    // Network broadcast handled by packet system
}

void DamageSystem::BroadcastKillEvent(const KillEvent& /*event*/) const {
    // Network broadcast handled by packet system
}
