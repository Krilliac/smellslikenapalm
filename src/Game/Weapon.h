// src/Game/Weapon.h – Header for Weapon

#pragma once

#include <string>
#include <vector>
#include "Math/Vector3.h"

enum class FireMode {
    SemiAuto,
    Burst,
    FullAuto
};

struct WeaponStats {
    float damage;
    float range;
    float accuracy;
    float fireRate;      // rounds per minute
    float reloadTime;    // seconds
    int   magazineSize;
    int   maxAmmo;
    float recoil;
    float weight;
};

class Player;

class Weapon {
public:
    Weapon(const std::string& name, const WeaponStats& stats);
    ~Weapon();

    // Firing
    bool CanFire() const;
    void Fire(const Vector3& origin, const Vector3& direction, Player* shooter);
    void UpdateFireTimer(float deltaSeconds);

    // Reloading
    bool NeedsReload() const;
    void Reload();
    void UpdateReloadTimer(float deltaSeconds);

    // Ammo management
    int  GetAmmoInMagazine() const;
    int  GetRemainingAmmo() const;
    bool AddAmmo(int amount);

    // Accessors
    const std::string& GetName() const;
    const WeaponStats& GetStats() const;
    FireMode           GetFireMode() const;
    void               SetFireMode(FireMode mode);

private:
    std::string  m_name;
    WeaponStats  m_stats;
    FireMode     m_fireMode;

    int          m_ammoInMagazine;
    int          m_remainingAmmo;

    // Timing
    float        m_timeSinceLastShot;
    bool         m_isReloading;
    float        m_reloadTimer;

    // Helpers
    float ShotsPerSecond() const;
    void SpawnProjectile(const Vector3& origin, const Vector3& direction, Player* shooter);
};