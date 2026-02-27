// src/Game/DamageSystem.h
// RS2V damage processing — hit detection, damage calculation, kill tracking

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "Math/Vector3.h"

class GameServer;

// Body part hit zones
enum class HitZone : uint8_t {
    Head,
    Chest,
    Stomach,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg
};

// Damage source types
enum class DamageSource : uint8_t {
    Bullet,
    Explosion,
    Fire,           // Napalm/flamethrower
    Melee,
    Bayonet,
    Vehicle,        // Run over
    Falling,
    Drowning,
    Artillery,
    Trap            // Punji sticks/mines
};

// Individual damage event
struct DamageEvent {
    uint32_t attackerId = 0;
    uint32_t victimId = 0;
    DamageSource source = DamageSource::Bullet;
    std::string weaponId;
    HitZone hitZone = HitZone::Chest;
    float baseDamage = 0.0f;
    float finalDamage = 0.0f;
    float distance = 0.0f;
    Vector3 hitPosition;
    Vector3 hitDirection;
    bool isFriendlyFire = false;
    bool isHeadshot = false;
    bool isKill = false;
};

// Kill event for scoring
struct KillEvent {
    uint32_t killerId = 0;
    uint32_t victimId = 0;
    std::string weaponId;
    DamageSource source = DamageSource::Bullet;
    float distance = 0.0f;
    bool isHeadshot = false;
    bool isFriendlyFire = false;
    bool isTeamKill = false;
};

using DamageCallback = std::function<void(const DamageEvent&)>;
using KillCallback = std::function<void(const KillEvent&)>;

class DamageSystem {
public:
    explicit DamageSystem(GameServer* server);
    ~DamageSystem();

    void Initialize();
    void Shutdown();

    // Process a damage event
    void ApplyDamage(const DamageEvent& event);

    // Explosion damage (affects all players in radius)
    void ApplyExplosionDamage(uint32_t attackerId, const std::string& weaponId,
                              const Vector3& center, float radius,
                              float maxDamage, DamageSource source);

    // Fire/napalm damage over time
    void ApplyFireDamage(uint32_t attackerId, const Vector3& center,
                         float radius, float damagePerSecond, float duration);

    // Hit zone damage multiplier
    float GetHitZoneMultiplier(HitZone zone) const;

    // Friendly fire check
    bool IsFriendlyFire(uint32_t attackerId, uint32_t victimId) const;

    // Callbacks
    void SetOnDamage(DamageCallback cb);
    void SetOnKill(KillCallback cb);

    // Per-tick update (processes fire DoT effects)
    void Update(float deltaSeconds);

    // Configuration
    void SetFriendlyFireEnabled(bool enabled);
    void SetFriendlyFireDamageScale(float scale);

private:
    GameServer* m_server;
    bool m_friendlyFireEnabled = true;
    float m_friendlyFireScale = 0.5f;  // 50% damage for friendly fire

    DamageCallback m_damageCallback;
    KillCallback m_killCallback;

    // Active fire effects
    struct FireEffect {
        uint32_t attackerId;
        Vector3 center;
        float radius;
        float damagePerSecond;
        float remaining;
        float tickTimer;
    };
    std::vector<FireEffect> m_activeFireEffects;

    void ProcessKill(const DamageEvent& event);
    float CalculateFinalDamage(DamageEvent& event) const;
    void BroadcastDamageEvent(const DamageEvent& event) const;
    void BroadcastKillEvent(const KillEvent& event) const;
};
