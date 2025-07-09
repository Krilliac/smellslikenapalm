// src/Game/Weapon.cpp – Implementation for Weapon

#include "Game/Weapon.h"
#include "Utils/Logger.h"
#include "Game/Player.h"
#include "Network/NetworkManager.h"
#include <algorithm>

Weapon::Weapon(const std::string& name, const WeaponStats& stats)
    : m_name(name)
    , m_stats(stats)
    , m_fireMode(FireMode::SemiAuto)
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
    // For hitscan or projectile; send network packet or instantiate in world
    if (auto net = shooter->GetConnection()->GetNetworkManager()) {
        // Example packet: FIRE_WEAPON, weapon name, origin, direction
        std::vector<uint8_t> data;
        // serialize strings and vectors here...
        net->SendPacketToAll("FIRE_WEAPON", data);
    }
    // Actual game logic would handle hit detection, damage application, recoil, etc.
}