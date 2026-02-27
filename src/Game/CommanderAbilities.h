// src/Game/CommanderAbilities.h
// RS2V commander fire support and ability system

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <functional>
#include "Math/Vector3.h"
#include "Game/RoleSystem.h"

class GameServer;

// Types of commander abilities available
enum class AbilityType : uint8_t {
    // US/Allied Fire Support
    Artillery,          // 105mm howitzer barrage (US)
    Napalm,             // F-4 Phantom napalm strike (US)
    AntiAirArtillery,   // Quad .50 cal anti-air (US)
    Spooky,             // AC-47 Spooky gunship loiter (US)
    CarpetBomb,         // B-52 Stratofortress carpet bomb (US) — high level commander
    ReconPlane,         // Reconnaissance flyover (US)

    // NVA/VC Fire Support
    MortarBarrage,      // 82mm mortar barrage (NVA)
    RocketBarrage,      // 122mm Katyusha rocket barrage (NVA)
    AntiAirMissile,     // SA-7 Grail anti-air missile (NVA)
    HoChiMinhTrail,     // Rapid resupply / reinforcements (NVA)
    AmbushTrap,         // Punji stick trap deployment (VC)

    // Shared
    ForceRespawn,       // Force all dead players to respawn
    ReconReport,        // Reveal enemy positions briefly
    Resupply            // Ammo crate drop
};

// State of an ability cooldown
struct AbilityCooldown {
    AbilityType type;
    float cooldownSeconds = 0.0f;       // Total cooldown time
    float remainingSeconds = 0.0f;      // Remaining until available
    bool isAvailable = true;
    int usesRemaining = -1;             // -1 = unlimited
};

// An active fire support effect in the world
struct ActiveFireSupport {
    uint32_t id = 0;
    AbilityType type;
    uint32_t commanderId = 0;
    uint32_t teamId = 0;
    Vector3 targetPosition;
    Vector3 targetDirection;            // For directional strikes like napalm
    float radius = 50.0f;
    float duration = 10.0f;             // seconds
    float elapsed = 0.0f;
    float damagePerSecond = 200.0f;
    float tickInterval = 0.5f;          // damage tick interval
    float tickAccumulator = 0.0f;
    bool active = true;

    // For multi-wave effects (artillery)
    int totalWaves = 1;
    int currentWave = 0;
    float timeBetweenWaves = 2.0f;
};

using FireSupportCallback = std::function<void(const ActiveFireSupport& support)>;

class CommanderAbilities {
public:
    explicit CommanderAbilities(GameServer* server);
    ~CommanderAbilities();

    void Initialize();
    void Shutdown();

    // Request fire support (commander action)
    bool RequestAbility(uint32_t commanderId, AbilityType type, const Vector3& target, const Vector3& direction = {});

    // Per-tick update: process active fire support effects
    void Update(float deltaSeconds);

    // Query
    bool IsAbilityAvailable(uint32_t teamId, AbilityType type) const;
    float GetCooldownRemaining(uint32_t teamId, AbilityType type) const;
    std::vector<AbilityCooldown> GetTeamAbilities(uint32_t teamId) const;
    std::vector<const ActiveFireSupport*> GetActiveEffects() const;
    std::string GetAbilityName(AbilityType type) const;

    // Callbacks
    void SetOnFireSupportActive(FireSupportCallback cb);

    // Configuration
    void SetRequiresRadioman(bool value);
    bool GetRequiresRadioman() const;

private:
    GameServer* m_server;

    // Per-team ability cooldowns
    std::map<uint32_t, std::vector<AbilityCooldown>> m_teamCooldowns;

    // Active fire support effects
    std::vector<ActiveFireSupport> m_activeEffects;
    uint32_t m_nextEffectId = 1;

    // Config
    bool m_requiresRadioman = true;  // Commander must be near radioman to call support

    // Callbacks
    FireSupportCallback m_fireSupportCallback;

    void InitializeTeamAbilities(uint32_t teamId, Faction faction);
    void ApplyFireSupportDamage(const ActiveFireSupport& effect);
    void BroadcastFireSupportEvent(const ActiveFireSupport& effect) const;
    AbilityCooldown* FindCooldown(uint32_t teamId, AbilityType type);
    const AbilityCooldown* FindCooldown(uint32_t teamId, AbilityType type) const;
    ActiveFireSupport CreateEffect(AbilityType type, uint32_t commanderId,
                                    uint32_t teamId, const Vector3& target, const Vector3& direction);
};
