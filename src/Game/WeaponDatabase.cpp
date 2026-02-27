// src/Game/WeaponDatabase.cpp
// RS2V weapon database — all weapon definitions with realistic ballistic parameters

#include "Game/WeaponDatabase.h"
#include "Utils/Logger.h"
#include <cmath>
#include <algorithm>

WeaponDatabase::WeaponDatabase() {}
WeaponDatabase::~WeaponDatabase() { Shutdown(); }

void WeaponDatabase::Initialize() {
    m_weapons.clear();
    RegisterUSWeapons();
    RegisterNVAWeapons();
    RegisterSharedWeapons();
    RegisterExplosives();
    Logger::Info("WeaponDatabase initialized with %zu weapons", m_weapons.size());
}

void WeaponDatabase::Shutdown() {
    m_weapons.clear();
}

void WeaponDatabase::RegisterWeapon(const WeaponDefinition& def) {
    m_weapons[def.id] = def;
}

const WeaponDefinition* WeaponDatabase::GetWeapon(const std::string& id) const {
    auto it = m_weapons.find(id);
    return it != m_weapons.end() ? &it->second : nullptr;
}

std::vector<const WeaponDefinition*> WeaponDatabase::GetWeaponsByCategory(WeaponCategory cat) const {
    std::vector<const WeaponDefinition*> result;
    for (const auto& [id, def] : m_weapons) {
        if (def.category == cat) result.push_back(&def);
    }
    return result;
}

std::vector<const WeaponDefinition*> WeaponDatabase::GetWeaponsByFaction(Faction faction) const {
    std::vector<const WeaponDefinition*> result;
    for (const auto& [id, def] : m_weapons) {
        if (def.availableFactions == faction || def.availableFactions == Faction::None) {
            result.push_back(&def);
        }
    }
    return result;
}

std::vector<const WeaponDefinition*> WeaponDatabase::GetAllWeapons() const {
    std::vector<const WeaponDefinition*> result;
    result.reserve(m_weapons.size());
    for (const auto& [id, def] : m_weapons) {
        result.push_back(&def);
    }
    return result;
}

std::unique_ptr<Weapon> WeaponDatabase::CreateWeaponInstance(const std::string& id) const {
    auto* def = GetWeapon(id);
    if (!def) return nullptr;
    auto weapon = std::make_unique<Weapon>(def->displayName, def->stats);
    if (!def->supportedFireModes.empty()) {
        weapon->SetFireMode(def->supportedFireModes.front());
    }
    return weapon;
}

float WeaponDatabase::CalculateDamage(const std::string& weaponId, float distance, bool isHeadshot, bool isLimb) const {
    auto* def = GetWeapon(weaponId);
    if (!def) return 0.0f;

    float baseDmg = def->stats.damage;
    float falloff = CalculateDamageFalloff(*def, distance);
    float dmg = baseDmg * falloff;

    // RS2V damage model: energy transfer based on deceleration
    // At very close range, bullets may over-penetrate (less damage)
    // Peak damage occurs at medium range (~20-50m)
    if (def->ballistics.ammoType != AmmoType::Shotgun_12ga &&
        def->ballistics.ammoType != AmmoType::Flame) {
        float overpenFactor = 1.0f;
        if (distance < 5.0f) {
            overpenFactor = 0.85f + 0.03f * distance;  // 85-100% at 0-5m
        } else if (distance < 50.0f) {
            overpenFactor = 1.0f;  // Peak energy transfer
        }
        dmg *= overpenFactor;
    }

    // Hit zone multipliers
    if (isHeadshot) {
        dmg *= def->ballistics.headshotMultiplier;
    } else if (isLimb) {
        dmg *= def->ballistics.limbMultiplier;
    }

    return dmg;
}

float WeaponDatabase::CalculateDamageFalloff(const WeaponDefinition& def, float distance) const {
    // Damage falls off based on effective range
    float effectiveRange = def.stats.range;
    if (distance <= effectiveRange * 0.5f) return 1.0f;
    if (distance >= effectiveRange) return 0.3f;  // Minimum damage at max range

    float t = (distance - effectiveRange * 0.5f) / (effectiveRange * 0.5f);
    return 1.0f - 0.7f * t;  // Linear falloff from 100% to 30%
}

float WeaponDatabase::CalculateBulletTravelTime(const WeaponDefinition& def, float distance) const {
    if (def.ballistics.isProjectile) {
        return def.ballistics.projectileSpeed > 0 ? distance / def.ballistics.projectileSpeed : 0.0f;
    }
    return def.ballistics.muzzleVelocity > 0 ? distance / def.ballistics.muzzleVelocity : 0.0f;
}

// ============================================================================
// US / Allied Weapons
// ============================================================================
void WeaponDatabase::RegisterUSWeapons() {
    // --- M16A1 (5.56x45mm NATO) ---
    {
        WeaponDefinition w;
        w.id = "M16A1";
        w.displayName = "M16A1";
        w.description = "Standard US Army/USMC assault rifle. Lightweight with high velocity 5.56mm rounds.";
        w.category = WeaponCategory::AssaultRifle;
        w.availableFactions = Faction::USArmy;
        w.stats = {55.0f, 460.0f, 0.85f, 700.0f, 3.2f, 20, 200, 1.8f, 3.1f};
        w.ballistics = {AmmoType::Rifle_556, 948.0f, 9.81f, 0.6f, 0.8f, 10.0f, 0.4f, 2.5f, 0.25f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto, FireMode::FullAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.25f;
        RegisterWeapon(w);
    }

    // --- XM177E2 (5.56mm carbine) ---
    {
        WeaponDefinition w;
        w.id = "XM177E2";
        w.displayName = "XM177E2";
        w.description = "Compact carbine variant of the M16. Used by special forces and squad leaders.";
        w.category = WeaponCategory::Carbine;
        w.availableFactions = Faction::USArmy;
        w.stats = {50.0f, 350.0f, 0.78f, 750.0f, 3.0f, 20, 160, 2.2f, 2.7f};
        w.ballistics = {AmmoType::Rifle_556, 838.0f, 9.81f, 0.5f, 0.7f, 10.0f, 0.4f, 2.0f, 0.2f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto, FireMode::FullAuto};
        w.aimDownSightTime = 0.22f;
        RegisterWeapon(w);
    }

    // --- M14 (7.62x51mm NATO) ---
    {
        WeaponDefinition w;
        w.id = "M14";
        w.displayName = "M14";
        w.description = "Battle rifle with powerful 7.62mm NATO rounds. High damage, strong recoil.";
        w.category = WeaponCategory::BattleRifle;
        w.availableFactions = Faction::USArmy;
        w.stats = {85.0f, 700.0f, 0.88f, 700.0f, 3.5f, 20, 200, 3.0f, 4.1f};
        w.ballistics = {AmmoType::Rifle_762NATO, 853.0f, 9.81f, 1.2f, 1.0f, 10.0f, 0.5f, 3.0f, 0.35f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto, FireMode::FullAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.3f;
        RegisterWeapon(w);
    }

    // --- M40 Sniper Rifle (7.62 NATO, bolt action) ---
    {
        WeaponDefinition w;
        w.id = "M40";
        w.displayName = "M40";
        w.description = "USMC bolt-action sniper rifle. Accurate and lethal at long range.";
        w.category = WeaponCategory::SniperRifle;
        w.availableFactions = Faction::USMC;
        w.stats = {95.0f, 900.0f, 0.98f, 30.0f, 4.5f, 5, 40, 4.0f, 4.5f};
        w.ballistics = {AmmoType::Rifle_762NATO, 777.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 3.5f, 0.4f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.4f;
        RegisterWeapon(w);
    }

    // --- XM21 (7.62 NATO, semi-auto sniper) ---
    {
        WeaponDefinition w;
        w.id = "XM21";
        w.displayName = "XM21";
        w.description = "Semi-automatic sniper variant of the M14 with 3-9x scope.";
        w.category = WeaponCategory::SniperRifle;
        w.availableFactions = Faction::USArmy;
        w.stats = {85.0f, 800.0f, 0.95f, 120.0f, 3.5f, 20, 100, 3.2f, 4.3f};
        w.ballistics = {AmmoType::Rifle_762NATO, 853.0f, 9.81f, 1.2f, 1.0f, 10.0f, 0.5f, 3.5f, 0.35f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.35f;
        RegisterWeapon(w);
    }

    // --- M60 (7.62 NATO GPMG) ---
    {
        WeaponDefinition w;
        w.id = "M60";
        w.displayName = "M60";
        w.description = "US general-purpose machine gun. Devastating suppressive fire with bipod.";
        w.category = WeaponCategory::MediumMachineGun;
        w.availableFactions = Faction::USArmy;
        w.stats = {80.0f, 700.0f, 0.72f, 550.0f, 7.0f, 100, 400, 3.5f, 10.4f};
        w.ballistics = {AmmoType::Rifle_762NATO, 853.0f, 9.81f, 1.2f, 1.0f, 10.0f, 0.5f, 5.0f, 0.5f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 1.5f;
        w.aimDownSightTime = 0.45f;
        w.sprintToFireTime = 0.8f;
        RegisterWeapon(w);
    }

    // --- M1919A6 (Browning .30 cal) ---
    {
        WeaponDefinition w;
        w.id = "M1919A6";
        w.displayName = "M1919A6";
        w.description = "WWII-era Browning .30 cal machine gun. Still in ARVN service.";
        w.category = WeaponCategory::MediumMachineGun;
        w.availableFactions = Faction::USArmy;
        w.stats = {78.0f, 650.0f, 0.70f, 500.0f, 8.0f, 250, 500, 3.0f, 14.3f};
        w.ballistics = {AmmoType::Rifle_3006, 854.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 5.0f, 0.5f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 2.0f;
        w.aimDownSightTime = 0.5f;
        RegisterWeapon(w);
    }

    // --- BAR (M1918A2) ---
    {
        WeaponDefinition w;
        w.id = "BAR";
        w.displayName = "M1918A2 BAR";
        w.description = "Browning Automatic Rifle. Heavy but reliable automatic fire.";
        w.category = WeaponCategory::LightMachineGun;
        w.availableFactions = Faction::USArmy;
        w.stats = {82.0f, 600.0f, 0.75f, 500.0f, 3.5f, 20, 200, 3.2f, 7.9f};
        w.ballistics = {AmmoType::Rifle_3006, 854.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 4.0f, 0.4f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 1.0f;
        RegisterWeapon(w);
    }

    // --- M79 Grenade Launcher ---
    {
        WeaponDefinition w;
        w.id = "M79";
        w.displayName = "M79 'Blooper'";
        w.description = "Single-shot 40mm grenade launcher. Devastating against groups.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::USArmy;
        w.stats = {150.0f, 350.0f, 0.70f, 6.0f, 3.0f, 1, 24, 1.5f, 2.7f};
        w.ballistics = {AmmoType::Explosive_40mm, 76.0f, 9.81f, 0.0f, 2.0f, 1.0f, 1.0f, 8.0f, 0.0f, true, 76.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- M3A1 Grease Gun (.45 ACP SMG) ---
    {
        WeaponDefinition w;
        w.id = "M3A1";
        w.displayName = "M3A1 Grease Gun";
        w.description = "Suppressed .45 ACP submachine gun. Slow rate of fire but good stopping power.";
        w.category = WeaponCategory::SubMachineGun;
        w.availableFactions = Faction::USArmy;
        w.stats = {55.0f, 150.0f, 0.72f, 450.0f, 3.0f, 30, 180, 1.2f, 3.6f};
        w.ballistics = {AmmoType::Pistol_45ACP, 280.0f, 9.81f, 0.3f, 0.6f, 10.0f, 0.45f, 2.0f, 0.15f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.aimDownSightTime = 0.2f;
        RegisterWeapon(w);
    }

    // --- Ithaca 37 (12ga shotgun) ---
    {
        WeaponDefinition w;
        w.id = "Ithaca37";
        w.displayName = "Ithaca 37";
        w.description = "Pump-action 12-gauge shotgun. Devastating at close range.";
        w.category = WeaponCategory::Shotgun;
        w.availableFactions = Faction::USArmy;
        w.stats = {120.0f, 40.0f, 0.5f, 70.0f, 4.5f, 4, 32, 2.0f, 3.0f};
        w.ballistics = {AmmoType::Shotgun_12ga, 400.0f, 9.81f, 0.2f, 0.5f, 2.0f, 0.8f, 1.5f, 0.1f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.2f;
        RegisterWeapon(w);
    }

    // --- M1911A1 (.45 ACP pistol) ---
    {
        WeaponDefinition w;
        w.id = "M1911";
        w.displayName = "M1911A1";
        w.description = "Standard US sidearm. Reliable .45 caliber pistol.";
        w.category = WeaponCategory::Pistol;
        w.availableFactions = Faction::USArmy;
        w.stats = {55.0f, 75.0f, 0.7f, 120.0f, 2.0f, 7, 42, 1.5f, 1.1f};
        w.ballistics = {AmmoType::Pistol_45ACP, 253.0f, 9.81f, 0.3f, 0.5f, 5.0f, 0.45f, 1.0f, 0.1f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.15f;
        RegisterWeapon(w);
    }

    // --- M2 Flamethrower ---
    {
        WeaponDefinition w;
        w.id = "M2Flamethrower";
        w.displayName = "M2 Flamethrower";
        w.description = "Infantry flamethrower. Terrifying area denial weapon.";
        w.category = WeaponCategory::Flamethrower;
        w.availableFactions = Faction::USArmy;
        w.stats = {200.0f, 25.0f, 1.0f, 300.0f, 0.0f, 100, 100, 0.0f, 15.0f};
        w.ballistics = {AmmoType::Flame, 15.0f, 0.0f, 0.0f, 3.0f, 1.0f, 1.0f, 5.0f, 0.0f, true, 15.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        RegisterWeapon(w);
    }

    // --- M1 Garand (.30-06, ARVN) ---
    {
        WeaponDefinition w;
        w.id = "M1Garand";
        w.displayName = "M1 Garand";
        w.description = "WWII-era semi-automatic rifle. 8-round en-bloc clip. Still issued to ARVN.";
        w.category = WeaponCategory::SemiAutoRifle;
        w.availableFactions = Faction::USArmy;
        w.stats = {88.0f, 600.0f, 0.90f, 40.0f, 4.0f, 8, 80, 2.8f, 4.3f};
        w.ballistics = {AmmoType::Rifle_3006, 854.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 3.0f, 0.35f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.3f;
        RegisterWeapon(w);
    }

    // --- L1A1 SLR (7.62 NATO, Australian) ---
    {
        WeaponDefinition w;
        w.id = "L1A1";
        w.displayName = "L1A1 SLR";
        w.description = "Australian variant of the FN FAL. Reliable semi-auto battle rifle.";
        w.category = WeaponCategory::BattleRifle;
        w.availableFactions = Faction::AusArmy;
        w.stats = {85.0f, 700.0f, 0.88f, 60.0f, 3.5f, 20, 200, 2.8f, 4.3f};
        w.ballistics = {AmmoType::Rifle_762NATO, 838.0f, 9.81f, 1.2f, 1.0f, 10.0f, 0.5f, 3.0f, 0.35f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.3f;
        RegisterWeapon(w);
    }

    // --- M72 LAW (anti-tank) ---
    {
        WeaponDefinition w;
        w.id = "M72LAW";
        w.displayName = "M72 LAW";
        w.description = "Disposable anti-tank rocket launcher. Effective against vehicles and bunkers.";
        w.category = WeaponCategory::RocketLauncher;
        w.availableFactions = Faction::USArmy;
        w.stats = {300.0f, 200.0f, 0.6f, 6.0f, 0.0f, 1, 1, 0.0f, 2.5f};
        w.ballistics = {AmmoType::Explosive_LAW, 0.0f, 9.81f, 3.0f, 5.0f, 1.0f, 1.0f, 10.0f, 0.0f, true, 145.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }
}

// ============================================================================
// NVA / VC Weapons
// ============================================================================
void WeaponDatabase::RegisterNVAWeapons() {
    // --- AK-47 (Type 56, 7.62x39mm) ---
    {
        WeaponDefinition w;
        w.id = "AK47";
        w.displayName = "AK-47";
        w.description = "Reliable Soviet assault rifle. Moderate accuracy, devastating close-medium range.";
        w.category = WeaponCategory::AssaultRifle;
        w.availableFactions = Faction::NVA;
        w.stats = {70.0f, 400.0f, 0.75f, 600.0f, 3.0f, 30, 210, 2.5f, 3.5f};
        w.ballistics = {AmmoType::Rifle_762x39, 715.0f, 9.81f, 0.9f, 0.9f, 10.0f, 0.5f, 3.0f, 0.3f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto, FireMode::FullAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.28f;
        RegisterWeapon(w);
    }

    // --- SKS (7.62x39mm semi-auto) ---
    {
        WeaponDefinition w;
        w.id = "SKS";
        w.displayName = "SKS";
        w.description = "Semi-automatic carbine. Accurate with integral 10-round stripper clip.";
        w.category = WeaponCategory::SemiAutoRifle;
        w.availableFactions = Faction::NVA;
        w.stats = {70.0f, 450.0f, 0.85f, 40.0f, 4.0f, 10, 100, 2.0f, 3.6f};
        w.ballistics = {AmmoType::Rifle_762x39, 735.0f, 9.81f, 0.9f, 0.9f, 10.0f, 0.5f, 2.5f, 0.25f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.28f;
        RegisterWeapon(w);
    }

    // --- Mosin-Nagant M91/30 (7.62x54R bolt action) ---
    {
        WeaponDefinition w;
        w.id = "MosinNagant";
        w.displayName = "Mosin-Nagant M91/30";
        w.description = "WWII-era bolt-action rifle. Powerful 7.62x54R rounds, 1-shot kill to torso.";
        w.category = WeaponCategory::BoltActionRifle;
        w.availableFactions = Faction::NVA;
        w.stats = {95.0f, 800.0f, 0.92f, 15.0f, 4.5f, 5, 50, 3.5f, 4.0f};
        w.ballistics = {AmmoType::Rifle_762x54R, 808.0f, 9.81f, 1.4f, 1.2f, 10.0f, 0.5f, 3.0f, 0.35f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.hasBayonet = true;
        w.aimDownSightTime = 0.35f;
        RegisterWeapon(w);
    }

    // --- Mosin-Nagant PU Sniper (scoped) ---
    {
        WeaponDefinition w;
        w.id = "MosinPU";
        w.displayName = "Mosin-Nagant PU";
        w.description = "Scoped Mosin-Nagant with PU 3.5x scope. NVA/VC sniper weapon.";
        w.category = WeaponCategory::SniperRifle;
        w.availableFactions = Faction::NVA;
        w.stats = {95.0f, 900.0f, 0.96f, 15.0f, 4.5f, 5, 50, 3.5f, 4.2f};
        w.ballistics = {AmmoType::Rifle_762x54R, 808.0f, 9.81f, 1.4f, 1.2f, 10.0f, 0.5f, 3.5f, 0.4f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.4f;
        RegisterWeapon(w);
    }

    // --- SVD Dragunov (7.62x54R semi-auto sniper) ---
    {
        WeaponDefinition w;
        w.id = "SVD";
        w.displayName = "SVD Dragunov";
        w.description = "Soviet designated marksman rifle. Semi-auto with PSO-1 4x scope.";
        w.category = WeaponCategory::SniperRifle;
        w.availableFactions = Faction::NVA;
        w.stats = {90.0f, 850.0f, 0.94f, 30.0f, 3.5f, 10, 60, 3.0f, 4.3f};
        w.ballistics = {AmmoType::Rifle_762x54R, 830.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 3.5f, 0.4f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.35f;
        RegisterWeapon(w);
    }

    // --- RPD (7.62x39mm LMG) ---
    {
        WeaponDefinition w;
        w.id = "RPD";
        w.displayName = "RPD";
        w.description = "Soviet light machine gun. Belt-fed from drum. NVA standard LMG.";
        w.category = WeaponCategory::LightMachineGun;
        w.availableFactions = Faction::NVA;
        w.stats = {70.0f, 500.0f, 0.70f, 650.0f, 8.0f, 100, 300, 2.8f, 7.4f};
        w.ballistics = {AmmoType::Rifle_762x39, 735.0f, 9.81f, 0.9f, 0.9f, 10.0f, 0.5f, 4.5f, 0.45f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 1.5f;
        w.aimDownSightTime = 0.4f;
        RegisterWeapon(w);
    }

    // --- DP-28 (7.62x54R LMG) ---
    {
        WeaponDefinition w;
        w.id = "DP28";
        w.displayName = "DP-28";
        w.description = "WWII-era Soviet LMG with 47-round pan magazine. Strong 7.62x54R cartridge.";
        w.category = WeaponCategory::LightMachineGun;
        w.availableFactions = Faction::NLFSV;
        w.stats = {85.0f, 600.0f, 0.72f, 550.0f, 9.0f, 47, 188, 3.0f, 9.1f};
        w.ballistics = {AmmoType::Rifle_762x54R, 840.0f, 9.81f, 1.3f, 1.1f, 10.0f, 0.5f, 4.5f, 0.45f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 1.5f;
        RegisterWeapon(w);
    }

    // --- PPSh-41 (7.62x25mm SMG) ---
    {
        WeaponDefinition w;
        w.id = "PPSh41";
        w.displayName = "PPSh-41";
        w.description = "Soviet SMG with extreme rate of fire. 71-round drum or 35-round box magazine.";
        w.category = WeaponCategory::SubMachineGun;
        w.availableFactions = Faction::NLFSV;
        w.stats = {42.0f, 150.0f, 0.65f, 900.0f, 4.0f, 71, 213, 1.5f, 3.6f};
        w.ballistics = {AmmoType::Pistol_762x25, 488.0f, 9.81f, 0.4f, 0.6f, 5.0f, 0.45f, 2.0f, 0.2f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.aimDownSightTime = 0.2f;
        RegisterWeapon(w);
    }

    // --- MAT-49 (9mm SMG, French/VC) ---
    {
        WeaponDefinition w;
        w.id = "MAT49";
        w.displayName = "MAT-49";
        w.description = "French-made 9mm SMG captured during First Indochina War. VC sidearm.";
        w.category = WeaponCategory::SubMachineGun;
        w.availableFactions = Faction::NLFSV;
        w.stats = {40.0f, 120.0f, 0.68f, 600.0f, 3.5f, 32, 160, 1.3f, 3.5f};
        w.ballistics = {AmmoType::Pistol_9mm, 390.0f, 9.81f, 0.3f, 0.5f, 5.0f, 0.45f, 1.5f, 0.15f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.aimDownSightTime = 0.2f;
        RegisterWeapon(w);
    }

    // --- RPG-7 ---
    {
        WeaponDefinition w;
        w.id = "RPG7";
        w.displayName = "RPG-7";
        w.description = "Soviet rocket-propelled grenade launcher. Anti-vehicle and anti-fortification.";
        w.category = WeaponCategory::RocketLauncher;
        w.availableFactions = Faction::NVA;
        w.stats = {350.0f, 300.0f, 0.55f, 4.0f, 5.0f, 1, 5, 0.0f, 7.0f};
        w.ballistics = {AmmoType::Explosive_RPG, 0.0f, 9.81f, 4.0f, 6.0f, 1.0f, 1.0f, 12.0f, 0.0f, true, 115.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- MAS-49 (7.5mm French, with rifle grenades) ---
    {
        WeaponDefinition w;
        w.id = "MAS49";
        w.displayName = "MAS-49";
        w.description = "French semi-auto rifle. Sapper variant can launch rifle grenades.";
        w.category = WeaponCategory::SemiAutoRifle;
        w.availableFactions = Faction::NLFSV;
        w.stats = {75.0f, 500.0f, 0.82f, 40.0f, 3.5f, 10, 70, 2.2f, 3.9f};
        w.ballistics = {AmmoType::Rifle_762NATO, 823.0f, 9.81f, 1.0f, 0.9f, 10.0f, 0.5f, 2.5f, 0.3f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- Tokarev TT-33 (7.62x25mm pistol) ---
    {
        WeaponDefinition w;
        w.id = "TT33";
        w.displayName = "TT-33 Tokarev";
        w.description = "Soviet sidearm. High velocity 7.62x25mm round.";
        w.category = WeaponCategory::Pistol;
        w.availableFactions = Faction::NVA;
        w.stats = {45.0f, 60.0f, 0.65f, 120.0f, 2.0f, 8, 40, 1.2f, 0.85f};
        w.ballistics = {AmmoType::Pistol_762x25, 420.0f, 9.81f, 0.35f, 0.55f, 5.0f, 0.45f, 1.0f, 0.1f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.15f;
        RegisterWeapon(w);
    }

    // --- IZh-58 Double-Barrel Shotgun ---
    {
        WeaponDefinition w;
        w.id = "IZh58";
        w.displayName = "IZh-58";
        w.description = "Soviet double-barrel shotgun. Two rapid shots, slow reload.";
        w.category = WeaponCategory::Shotgun;
        w.availableFactions = Faction::NLFSV;
        w.stats = {130.0f, 35.0f, 0.45f, 120.0f, 5.0f, 2, 20, 2.5f, 3.1f};
        w.ballistics = {AmmoType::Shotgun_12ga, 400.0f, 9.81f, 0.2f, 0.5f, 2.0f, 0.8f, 1.5f, 0.1f, false, 0.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        w.aimDownSightTime = 0.2f;
        RegisterWeapon(w);
    }

    // --- DShK (12.7mm HMG, emplaced) ---
    {
        WeaponDefinition w;
        w.id = "DShK";
        w.displayName = "DShK";
        w.description = "Soviet 12.7mm heavy machine gun. Anti-aircraft and anti-vehicle capable.";
        w.category = WeaponCategory::HeavyMachineGun;
        w.availableFactions = Faction::NVA;
        w.stats = {150.0f, 1500.0f, 0.80f, 600.0f, 10.0f, 50, 250, 5.0f, 34.0f};
        w.ballistics = {AmmoType::Rifle_762x54R, 860.0f, 9.81f, 3.0f, 3.0f, 10.0f, 0.6f, 8.0f, 0.6f, false, 0.0f};
        w.supportedFireModes = {FireMode::FullAuto};
        w.deployTime = 0.0f;  // Emplaced
        RegisterWeapon(w);
    }
}

// ============================================================================
// Shared / Common Weapons
// ============================================================================
void WeaponDatabase::RegisterSharedWeapons() {
    // --- Knife/Bayonet (melee) ---
    {
        WeaponDefinition w;
        w.id = "Knife";
        w.displayName = "Knife";
        w.description = "Standard melee weapon. Silent and lethal up close.";
        w.category = WeaponCategory::Melee;
        w.availableFactions = Faction::None;  // Available to all
        w.stats = {100.0f, 2.0f, 1.0f, 60.0f, 0.0f, 1, 1, 0.0f, 0.3f};
        w.ballistics = {};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }
}

// ============================================================================
// Explosives / Grenades
// ============================================================================
void WeaponDatabase::RegisterExplosives() {
    // --- M26 Frag Grenade (US) ---
    {
        WeaponDefinition w;
        w.id = "M26Frag";
        w.displayName = "M26 Fragmentation Grenade";
        w.description = "Standard US fragmentation grenade. 4-5 second fuse.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::USArmy;
        w.stats = {250.0f, 40.0f, 0.7f, 10.0f, 0.0f, 1, 2, 0.0f, 0.4f};
        w.ballistics = {AmmoType::Explosive_40mm, 0.0f, 9.81f, 0.0f, 3.0f, 1.0f, 1.0f, 15.0f, 0.0f, true, 15.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- F1 Grenade (NVA/VC) ---
    {
        WeaponDefinition w;
        w.id = "F1Grenade";
        w.displayName = "F1 Fragmentation Grenade";
        w.description = "Soviet-era fragmentation grenade used by NVA/VC forces.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::NVA;
        w.stats = {240.0f, 35.0f, 0.65f, 10.0f, 0.0f, 1, 2, 0.0f, 0.6f};
        w.ballistics = {AmmoType::Explosive_40mm, 0.0f, 9.81f, 0.0f, 3.0f, 1.0f, 1.0f, 12.0f, 0.0f, true, 13.0f};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- M18 Smoke Grenade (US) ---
    {
        WeaponDefinition w;
        w.id = "M18Smoke";
        w.displayName = "M18 Smoke Grenade";
        w.description = "Colored smoke grenade for concealment and marking.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::USArmy;
        w.stats = {0.0f, 35.0f, 0.7f, 10.0f, 0.0f, 1, 2, 0.0f, 0.4f};
        w.ballistics = {};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- MD-82 Mine (VC Sapper) ---
    {
        WeaponDefinition w;
        w.id = "MD82Mine";
        w.displayName = "MD-82 Anti-Personnel Mine";
        w.description = "Directional fragmentation mine placed by Sappers.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::NLFSV;
        w.stats = {300.0f, 10.0f, 1.0f, 0.0f, 3.0f, 1, 5, 0.0f, 1.0f};
        w.ballistics = {};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- C4 Explosive (US Combat Engineer) ---
    {
        WeaponDefinition w;
        w.id = "C4";
        w.displayName = "C4 Explosive";
        w.description = "Plastic explosive charge. Remote detonated. Destroys structures and tunnels.";
        w.category = WeaponCategory::GrenadeLauncher;
        w.availableFactions = Faction::USArmy;
        w.stats = {500.0f, 10.0f, 1.0f, 0.0f, 3.0f, 1, 2, 0.0f, 1.0f};
        w.ballistics = {};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }

    // --- Punji Stake Trap (VC) ---
    {
        WeaponDefinition w;
        w.id = "PunjiTrap";
        w.displayName = "Punji Stake Trap";
        w.description = "Concealed bamboo spike trap. Causes bleeding and slow.";
        w.category = WeaponCategory::Melee;
        w.availableFactions = Faction::NLFSV;
        w.stats = {60.0f, 1.0f, 1.0f, 0.0f, 2.0f, 1, 3, 0.0f, 0.5f};
        w.ballistics = {};
        w.supportedFireModes = {FireMode::SemiAuto};
        RegisterWeapon(w);
    }
}
