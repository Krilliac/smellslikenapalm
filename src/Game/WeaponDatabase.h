// src/Game/WeaponDatabase.h
// RS2V weapon database — all weapon definitions with realistic ballistic parameters

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "Game/Weapon.h"
#include "Game/RoleSystem.h"

// Weapon categories matching RS2V
enum class WeaponCategory : uint8_t {
    AssaultRifle,
    BattleRifle,
    BoltActionRifle,
    SemiAutoRifle,
    Carbine,
    SubMachineGun,
    Shotgun,
    LightMachineGun,
    MediumMachineGun,
    HeavyMachineGun,
    SniperRifle,
    GrenadeLauncher,
    RocketLauncher,
    Pistol,
    Flamethrower,
    Melee
};

// Ammunition type affects penetration and damage falloff
enum class AmmoType : uint8_t {
    Pistol_45ACP,       // .45 ACP (M1911, M3 Grease Gun)
    Pistol_9mm,         // 9x19mm (various)
    Pistol_762x25,      // 7.62x25mm Tokarev
    Rifle_556,          // 5.56x45mm NATO (M16, XM177)
    Rifle_762NATO,      // 7.62x51mm NATO (M14, M60)
    Rifle_762x39,       // 7.62x39mm (AK-47, SKS, RPD)
    Rifle_762x54R,      // 7.62x54mmR (Mosin-Nagant, DP-28, PKM)
    Rifle_3006,         // .30-06 Springfield (M1 Garand, BAR)
    Shotgun_12ga,       // 12 gauge buckshot
    Shotgun_12gaSlug,   // 12 gauge slug
    Explosive_40mm,     // 40mm grenade (M79)
    Explosive_RPG,      // RPG-7 warhead
    Explosive_LAW,      // M72 LAW rocket
    Flame               // Flamethrower fuel
};

// Detailed ballistic profile
struct BallisticProfile {
    AmmoType ammoType = AmmoType::Rifle_556;
    float muzzleVelocity = 940.0f;      // m/s
    float bulletDrop = 9.81f;            // gravity effect (m/s^2)
    float penetration = 1.0f;            // material penetration factor (1.0 = standard)
    float armorDamageMultiplier = 1.0f;  // damage vs. armored targets
    float headshotMultiplier = 2.5f;     // headshot damage multiplier
    float limbMultiplier = 0.75f;        // limb hit damage multiplier
    float suppressionRadius = 3.0f;      // meters — suppression effect radius near misses
    float suppressionFactor = 0.3f;      // how much suppression (0-1)
    bool isProjectile = false;           // true = physical projectile, false = hitscan+drop
    float projectileSpeed = 0.0f;        // only used for rockets/grenades
};

// Complete weapon definition in the database
struct WeaponDefinition {
    std::string id;                      // Unique weapon identifier (e.g., "M16A1")
    std::string displayName;
    std::string description;
    WeaponCategory category = WeaponCategory::AssaultRifle;
    Faction availableFactions = Faction::None;  // Which faction(s) can use this
    WeaponStats stats;
    BallisticProfile ballistics;

    // Fire mode support
    std::vector<FireMode> supportedFireModes;

    // Visual/audio
    float aimDownSightTime = 0.3f;       // seconds to ADS
    float sprintToFireTime = 0.4f;       // seconds from sprint to fire
    float deployTime = 0.0f;             // bipod deploy time (MGs)

    // Bayonet
    bool hasBayonet = false;
    float bayonetDamage = 100.0f;        // one-hit kill
    float bayonetRange = 2.0f;
};

class WeaponDatabase {
public:
    WeaponDatabase();
    ~WeaponDatabase();

    void Initialize();
    void Shutdown();

    // Lookup
    const WeaponDefinition* GetWeapon(const std::string& id) const;
    std::vector<const WeaponDefinition*> GetWeaponsByCategory(WeaponCategory cat) const;
    std::vector<const WeaponDefinition*> GetWeaponsByFaction(Faction faction) const;
    std::vector<const WeaponDefinition*> GetAllWeapons() const;

    // Create a Weapon instance from a definition
    std::unique_ptr<Weapon> CreateWeaponInstance(const std::string& id) const;

    // Damage calculation with ballistics
    float CalculateDamage(const std::string& weaponId, float distance, bool isHeadshot, bool isLimb) const;
    float CalculateDamageFalloff(const WeaponDefinition& def, float distance) const;
    float CalculateBulletTravelTime(const WeaponDefinition& def, float distance) const;

private:
    std::unordered_map<std::string, WeaponDefinition> m_weapons;

    void RegisterUSWeapons();
    void RegisterNVAWeapons();
    void RegisterSharedWeapons();
    void RegisterExplosives();

    void RegisterWeapon(const WeaponDefinition& def);
};
