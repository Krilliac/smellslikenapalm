// src/Game/DamageSystem.cpp
// RS2V damage processing — hit detection, damage calculation, kill tracking

#include "Game/DamageSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Network/NetworkManager.h"
#include "Network/Packet.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>

DamageSystem::DamageSystem(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[DamageSystem::DamageSystem] Entry: server=%p", server);
    Logger::Trace("[DamageSystem::DamageSystem] Exit");
}

DamageSystem::~DamageSystem() {
    Logger::Trace("[DamageSystem::~DamageSystem] Entry");
    Shutdown();
    Logger::Trace("[DamageSystem::~DamageSystem] Exit");
}

void DamageSystem::Initialize() {
    Logger::Trace("[DamageSystem::Initialize] Entry");
    m_activeFireEffects.clear();
    Logger::Info("DamageSystem initialized");
    Logger::Trace("[DamageSystem::Initialize] Exit");
}

void DamageSystem::Shutdown() {
    Logger::Trace("[DamageSystem::Shutdown] Entry");
    Logger::Debug("[DamageSystem::Shutdown] Clearing %zu active fire effects", m_activeFireEffects.size());
    m_activeFireEffects.clear();
    Logger::Trace("[DamageSystem::Shutdown] Exit");
}

float DamageSystem::GetHitZoneMultiplier(HitZone zone) const {
    Logger::Trace("[DamageSystem::GetHitZoneMultiplier] Entry: zone=%d", static_cast<int>(zone));
    float multiplier;
    // RS2V damage model: head/spine = instant kill, chest = high, limbs = reduced
    switch (zone) {
        case HitZone::Head:      multiplier = 10.0f; break;
        case HitZone::Chest:     multiplier = 1.0f; break;
        case HitZone::Stomach:   multiplier = 0.9f; break;
        case HitZone::LeftArm:
        case HitZone::RightArm:  multiplier = 0.5f; break;
        case HitZone::LeftLeg:
        case HitZone::RightLeg:  multiplier = 0.4f; break;
        default:                 multiplier = 1.0f; break;
    }
    Logger::Debug("[DamageSystem::GetHitZoneMultiplier] zone=%d -> multiplier=%.1f", static_cast<int>(zone), multiplier);
    Logger::Trace("[DamageSystem::GetHitZoneMultiplier] Exit: return %.1f", multiplier);
    return multiplier;
}

bool DamageSystem::IsFriendlyFire(uint32_t attackerId, uint32_t victimId) const {
    Logger::Trace("[DamageSystem::IsFriendlyFire] Entry: attackerId=%u, victimId=%u", attackerId, victimId);
    if (attackerId == 0 || attackerId == victimId) {
        Logger::Debug("[DamageSystem::IsFriendlyFire] Self-damage or world damage (attackerId=%u), returning false", attackerId);
        Logger::Trace("[DamageSystem::IsFriendlyFire] Exit: return false");
        return false;
    }
    auto* tm = m_server->GetTeamManager();
    uint32_t attackerTeam = tm->GetPlayerTeam(attackerId);
    uint32_t victimTeam = tm->GetPlayerTeam(victimId);
    bool result = attackerTeam == victimTeam;
    Logger::Debug("[DamageSystem::IsFriendlyFire] attackerTeam=%u, victimTeam=%u, isFriendly=%d", attackerTeam, victimTeam, result);
    Logger::Trace("[DamageSystem::IsFriendlyFire] Exit: return %d", result);
    return result;
}

void DamageSystem::ApplyDamage(const DamageEvent& event) {
    Logger::Trace("[DamageSystem::ApplyDamage] Entry: attackerId=%u, victimId=%u, weaponId='%s', zone=%d, baseDamage=%.1f",
                  event.attackerId, event.victimId, event.weaponId.c_str(), static_cast<int>(event.hitZone), event.baseDamage);
    auto* pm = m_server->GetPlayerManager();
    auto victim = pm->GetPlayer(event.victimId);
    if (!victim || !victim->IsAlive()) {
        Logger::Debug("[DamageSystem::ApplyDamage] Victim %u is null or dead, skipping damage", event.victimId);
        Logger::Trace("[DamageSystem::ApplyDamage] Exit (victim invalid)");
        return;
    }

    DamageEvent e = event;
    e.isFriendlyFire = IsFriendlyFire(e.attackerId, e.victimId);
    e.isHeadshot = (e.hitZone == HitZone::Head);
    Logger::Debug("[DamageSystem::ApplyDamage] isFriendlyFire=%d, isHeadshot=%d", e.isFriendlyFire, e.isHeadshot);
    e.finalDamage = CalculateFinalDamage(e);
    Logger::Debug("[DamageSystem::ApplyDamage] finalDamage=%.1f (base=%.1f)", e.finalDamage, e.baseDamage);

    // Apply damage to victim
    int currentHp = victim->GetHealth();
    int newHp = currentHp - static_cast<int>(std::ceil(e.finalDamage));
    if (newHp < 0) newHp = 0;
    victim->SetHealth(newHp);
    Logger::Debug("[DamageSystem::ApplyDamage] Victim %u health: %d -> %d", e.victimId, currentHp, newHp);

    if (newHp <= 0) {
        e.isKill = true;
        Logger::Debug("[DamageSystem::ApplyDamage] Victim %u killed, processing kill event", e.victimId);
        ProcessKill(e);
    }

    if (m_damageCallback) {
        Logger::Debug("[DamageSystem::ApplyDamage] Invoking damage callback");
        m_damageCallback(e);
    }
    BroadcastDamageEvent(e);

    Logger::Debug("Damage: %u -> %u, weapon=%s, zone=%d, base=%.1f, final=%.1f, hp=%d->%d%s",
                  e.attackerId, e.victimId, e.weaponId.c_str(), static_cast<int>(e.hitZone),
                  e.baseDamage, e.finalDamage, currentHp, newHp,
                  e.isKill ? " [KILL]" : "");
    Logger::Trace("[DamageSystem::ApplyDamage] Exit");
}

void DamageSystem::ApplyExplosionDamage(uint32_t attackerId, const std::string& weaponId,
                                         const Vector3& center, float radius,
                                         float maxDamage, DamageSource source) {
    Logger::Trace("[DamageSystem::ApplyExplosionDamage] Entry: attackerId=%u, weaponId='%s', center=(%.1f,%.1f,%.1f), radius=%.1f, maxDamage=%.1f, source=%d",
                  attackerId, weaponId.c_str(), center.x, center.y, center.z, radius, maxDamage, static_cast<int>(source));
    auto* pm = m_server->GetPlayerManager();
    auto alivePlayers = pm->GetAlivePlayers();
    Logger::Debug("[DamageSystem::ApplyExplosionDamage] Checking %zu alive players against explosion", alivePlayers.size());
    int hitCount = 0;

    for (auto& player : alivePlayers) {
        uint32_t pid = player->GetConnection()->GetClientId();
        Vector3 playerPos = player->GetPosition();
        float dist = playerPos.Distance(center);

        if (dist > radius) continue;

        // Quadratic falloff from center
        float falloff = 1.0f - (dist / radius);
        falloff *= falloff;  // Quadratic
        float dmg = maxDamage * falloff;
        Logger::Debug("[DamageSystem::ApplyExplosionDamage] Player %u at dist=%.1f, falloff=%.2f, dmg=%.1f", pid, dist, falloff, dmg);

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
        hitCount++;
    }

    Logger::Info("[DamageSystem::ApplyExplosionDamage] Explosion hit %d players (radius=%.1f, maxDmg=%.1f)", hitCount, radius, maxDamage);
    Logger::Trace("[DamageSystem::ApplyExplosionDamage] Exit");
}

void DamageSystem::ApplyFireDamage(uint32_t attackerId, const Vector3& center,
                                    float radius, float damagePerSecond, float duration) {
    Logger::Trace("[DamageSystem::ApplyFireDamage] Entry: attackerId=%u, center=(%.1f,%.1f,%.1f), radius=%.1f, dps=%.1f, duration=%.1f",
                  attackerId, center.x, center.y, center.z, radius, damagePerSecond, duration);
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
    Logger::Info("[DamageSystem::ApplyFireDamage] Fire effect added, total active fire effects: %zu", m_activeFireEffects.size());
    Logger::Trace("[DamageSystem::ApplyFireDamage] Exit");
}

void DamageSystem::SetOnDamage(DamageCallback cb) {
    Logger::Trace("[DamageSystem::SetOnDamage] Entry");
    m_damageCallback = std::move(cb);
    Logger::Debug("[DamageSystem::SetOnDamage] Damage callback registered");
    Logger::Trace("[DamageSystem::SetOnDamage] Exit");
}

void DamageSystem::SetOnKill(KillCallback cb) {
    Logger::Trace("[DamageSystem::SetOnKill] Entry");
    m_killCallback = std::move(cb);
    Logger::Debug("[DamageSystem::SetOnKill] Kill callback registered");
    Logger::Trace("[DamageSystem::SetOnKill] Exit");
}

void DamageSystem::Update(float deltaSeconds) {
    // Hot loop - avoid per-tick Trace logging, but log significant events
    // Process fire DoT effects
    for (auto it = m_activeFireEffects.begin(); it != m_activeFireEffects.end();) {
        it->remaining -= deltaSeconds;
        it->tickTimer += deltaSeconds;

        if (it->remaining <= 0.0f) {
            Logger::Debug("[DamageSystem::Update] Fire effect expired at (%.1f,%.1f,%.1f)", it->center.x, it->center.y, it->center.z);
            it = m_activeFireEffects.erase(it);
            continue;
        }

        // Tick fire damage every 0.5 seconds
        if (it->tickTimer >= 0.5f) {
            it->tickTimer -= 0.5f;
            float tickDmg = it->damagePerSecond * 0.5f;
            Logger::Debug("[DamageSystem::Update] Fire tick damage: dps=%.1f, tickDmg=%.1f, remaining=%.1fs", it->damagePerSecond, tickDmg, it->remaining);
            ApplyExplosionDamage(it->attackerId, "Fire", it->center,
                                 it->radius, tickDmg, DamageSource::Fire);
        }
        ++it;
    }
}

void DamageSystem::SetFriendlyFireEnabled(bool enabled) {
    Logger::Trace("[DamageSystem::SetFriendlyFireEnabled] Entry: enabled=%d", enabled);
    m_friendlyFireEnabled = enabled;
    Logger::Info("[DamageSystem::SetFriendlyFireEnabled] Friendly fire %s", enabled ? "enabled" : "disabled");
    Logger::Trace("[DamageSystem::SetFriendlyFireEnabled] Exit");
}

void DamageSystem::SetFriendlyFireDamageScale(float scale) {
    Logger::Trace("[DamageSystem::SetFriendlyFireDamageScale] Entry: scale=%.2f", scale);
    m_friendlyFireScale = scale;
    Logger::Debug("[DamageSystem::SetFriendlyFireDamageScale] Friendly fire damage scale set to %.2f", scale);
    Logger::Trace("[DamageSystem::SetFriendlyFireDamageScale] Exit");
}

void DamageSystem::SetGlobalDamageScale(float scale) {
    Logger::Trace("[DamageSystem::SetGlobalDamageScale] Entry: scale=%.2f", scale);
    m_globalDamageScale = (scale < 0.0f) ? 0.0f : scale;
    Logger::Info("[DamageSystem::SetGlobalDamageScale] Global damage scale set to %.2f", m_globalDamageScale);
    Logger::Trace("[DamageSystem::SetGlobalDamageScale] Exit");
}

float DamageSystem::CalculateFinalDamage(DamageEvent& event) const {
    Logger::Trace("[DamageSystem::CalculateFinalDamage] Entry: baseDamage=%.1f, hitZone=%d, isFriendlyFire=%d",
                  event.baseDamage, static_cast<int>(event.hitZone), event.isFriendlyFire);
    float dmg = event.baseDamage;

    // Apply hit zone multiplier
    float zoneMultiplier = GetHitZoneMultiplier(event.hitZone);
    dmg *= zoneMultiplier;
    Logger::Debug("[DamageSystem::CalculateFinalDamage] After zone multiplier (%.1f): dmg=%.1f", zoneMultiplier, dmg);

    // Apply friendly fire scaling
    if (event.isFriendlyFire) {
        if (!m_friendlyFireEnabled) {
            Logger::Debug("[DamageSystem::CalculateFinalDamage] Friendly fire disabled, returning 0 damage");
            Logger::Trace("[DamageSystem::CalculateFinalDamage] Exit: return 0.0");
            return 0.0f;
        }
        dmg *= m_friendlyFireScale;
        Logger::Debug("[DamageSystem::CalculateFinalDamage] After friendly fire scale (%.2f): dmg=%.1f", m_friendlyFireScale, dmg);
    }

    float result = std::max(0.0f, dmg);
    Logger::Trace("[DamageSystem::CalculateFinalDamage] Exit: return %.1f", result);
    return result;
}

void DamageSystem::ProcessKill(const DamageEvent& event) {
    Logger::Trace("[DamageSystem::ProcessKill] Entry: killerId=%u, victimId=%u, weaponId='%s'", event.attackerId, event.victimId, event.weaponId.c_str());
    KillEvent kill;
    kill.killerId = event.attackerId;
    kill.victimId = event.victimId;
    kill.weaponId = event.weaponId;
    kill.source = event.source;
    kill.distance = event.distance;
    kill.isHeadshot = event.isHeadshot;
    kill.isFriendlyFire = event.isFriendlyFire;
    kill.isTeamKill = event.isFriendlyFire;

    Logger::Debug("[DamageSystem::ProcessKill] Kill event: isHeadshot=%d, isTK=%d, distance=%.1f", kill.isHeadshot, kill.isTeamKill, kill.distance);

    // Notify player manager
    auto* pm = m_server->GetPlayerManager();
    pm->OnPlayerDeath(event.victimId);

    if (m_killCallback) {
        Logger::Debug("[DamageSystem::ProcessKill] Invoking kill callback");
        m_killCallback(kill);
    }
    BroadcastKillEvent(kill);

    Logger::Info("Kill: %u killed %u with %s (dist=%.1fm%s%s)",
                 kill.killerId, kill.victimId, kill.weaponId.c_str(), kill.distance,
                 kill.isHeadshot ? ", headshot" : "",
                 kill.isTeamKill ? ", TEAMKILL" : "");
    Logger::Trace("[DamageSystem::ProcessKill] Exit");
}

void DamageSystem::BroadcastDamageEvent(const DamageEvent& event) const {
    Logger::Trace("[DamageSystem::BroadcastDamageEvent] Entry: attackerId=%u, victimId=%u", event.attackerId, event.victimId);

    auto* nm = m_server->GetNetworkManager();
    if (!nm) {
        Logger::Debug("[DamageSystem::BroadcastDamageEvent] No NetworkManager available");
        Logger::Trace("[DamageSystem::BroadcastDamageEvent] Exit (no NetworkManager)");
        return;
    }

    Packet pkt("DAMAGE_EVENT");
    pkt.WriteUInt(event.attackerId);
    pkt.WriteUInt(event.victimId);
    pkt.WriteFloat(event.finalDamage);
    pkt.WriteUInt(static_cast<uint32_t>(event.hitZone));
    pkt.WriteUInt(static_cast<uint32_t>(event.source));
    pkt.WriteVector3(event.hitPosition);
    pkt.WriteVector3(event.hitDirection);
    pkt.WriteUInt(event.isHeadshot ? 1 : 0);
    pkt.WriteUInt(event.isKill ? 1 : 0);
    nm->BroadcastPacket(pkt);

    Logger::Debug("[DamageSystem::BroadcastDamageEvent] Broadcast damage: %u->%u, dmg=%.1f, zone=%d, kill=%d",
                  event.attackerId, event.victimId, event.finalDamage,
                  static_cast<int>(event.hitZone), event.isKill);
    Logger::Trace("[DamageSystem::BroadcastDamageEvent] Exit");
}

void DamageSystem::BroadcastKillEvent(const KillEvent& event) const {
    Logger::Trace("[DamageSystem::BroadcastKillEvent] Entry: killerId=%u, victimId=%u", event.killerId, event.victimId);

    auto* nm = m_server->GetNetworkManager();
    if (!nm) {
        Logger::Debug("[DamageSystem::BroadcastKillEvent] No NetworkManager available");
        Logger::Trace("[DamageSystem::BroadcastKillEvent] Exit (no NetworkManager)");
        return;
    }

    Packet pkt("KILL_EVENT");
    pkt.WriteUInt(event.killerId);
    pkt.WriteUInt(event.victimId);
    pkt.WriteString(event.weaponId);
    pkt.WriteUInt(static_cast<uint32_t>(event.source));
    pkt.WriteFloat(event.distance);
    pkt.WriteUInt(event.isHeadshot ? 1 : 0);
    pkt.WriteUInt(event.isFriendlyFire ? 1 : 0);
    pkt.WriteUInt(event.isTeamKill ? 1 : 0);
    nm->BroadcastPacket(pkt);

    Logger::Debug("[DamageSystem::BroadcastKillEvent] Broadcast kill: %u killed %u with '%s'%s%s",
                  event.killerId, event.victimId, event.weaponId.c_str(),
                  event.isHeadshot ? " [HEADSHOT]" : "",
                  event.isTeamKill ? " [TK]" : "");
    Logger::Trace("[DamageSystem::BroadcastKillEvent] Exit");
}
