// src/Game/HelicopterPhysics.cpp
// RS2V helicopter flight model implementation

#include "Game/HelicopterPhysics.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include <cmath>
#include <algorithm>

static constexpr float GRAVITY = 9.81f;
static constexpr float DEG_TO_RAD = 3.14159265f / 180.0f;
static constexpr float RAD_TO_DEG = 180.0f / 3.14159265f;

HelicopterPhysics::HelicopterPhysics(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[HelicopterPhysics::HelicopterPhysics] entry, server=%p", static_cast<void*>(server));
    Logger::Trace("[HelicopterPhysics::HelicopterPhysics] exit");
}

HelicopterPhysics::~HelicopterPhysics() {
    Logger::Trace("[HelicopterPhysics::~HelicopterPhysics] entry");
    Shutdown();
    Logger::Trace("[HelicopterPhysics::~HelicopterPhysics] exit");
}

void HelicopterPhysics::Initialize() {
    Logger::Trace("[HelicopterPhysics::Initialize] entry");
    m_helicopters.clear();
    m_controlInputs.clear();
    m_flightModels.clear();
    Logger::Debug("[HelicopterPhysics::Initialize] cleared all containers");
    InitializeFlightModels();
    Logger::Info("HelicopterPhysics initialized");
    Logger::Trace("[HelicopterPhysics::Initialize] exit");
}

void HelicopterPhysics::Shutdown() {
    Logger::Trace("[HelicopterPhysics::Shutdown] entry");
    Logger::Info("[HelicopterPhysics::Shutdown] shutting down, clearing %zu helicopters and %zu control inputs",
                 m_helicopters.size(), m_controlInputs.size());
    m_helicopters.clear();
    m_controlInputs.clear();
    Logger::Trace("[HelicopterPhysics::Shutdown] exit");
}

void HelicopterPhysics::InitializeFlightModels() {
    Logger::Trace("[HelicopterPhysics::InitializeFlightModels] entry");
    // UH-1D/H Huey — transport
    {
        HeliFlightModel m;
        m.maxSpeed = 56.0f;          // ~110 knots
        m.maxClimbRate = 7.6f;
        m.maxDescentRate = 15.0f;
        m.cruiseSpeed = 45.0f;
        m.hoverStability = 0.85f;
        m.mainRotorRPM = 324.0f;
        m.rotorSpoolUpTime = 8.0f;
        m.rotorSpoolDownTime = 12.0f;
        m.pitchRate = 28.0f;
        m.rollRate = 35.0f;
        m.yawRate = 22.0f;
        m.collectiveRate = 0.5f;
        m.mass = 2365.0f;
        m.dragCoefficient = 0.42f;
        m.liftCoefficient = 1.0f;
        m.maxHealth = 500.0f;
        m.tailRotorHealth = 100.0f;
        m.engineHealth = 200.0f;
        m.fuelCapacity = 100.0f;
        m.fuelBurnRate = 0.08f;
        m.translationalLiftSpeed = 15.0f;
        m.vortexRingDescentRate = 10.0f;
        m.autorotateDescentRate = 12.0f;
        m.groundEffectHeight = 15.0f;
        m.groundEffectMultiplier = 1.3f;
        m_flightModels[static_cast<uint32_t>(HelicopterType::UH1_Huey)] = m;
        Logger::Debug("[HelicopterPhysics::InitializeFlightModels] registered UH1_Huey: maxSpeed=%.1f, mass=%.1f, maxHealth=%.1f",
                      m.maxSpeed, m.mass, m.maxHealth);
    }

    // OH-6A Loach — light observation
    {
        HeliFlightModel m;
        m.maxSpeed = 70.0f;          // ~135 knots — very fast
        m.maxClimbRate = 10.0f;
        m.maxDescentRate = 18.0f;
        m.cruiseSpeed = 55.0f;
        m.hoverStability = 0.75f;
        m.mainRotorRPM = 483.0f;
        m.rotorSpoolUpTime = 6.0f;
        m.rotorSpoolDownTime = 10.0f;
        m.pitchRate = 40.0f;
        m.rollRate = 50.0f;
        m.yawRate = 35.0f;
        m.collectiveRate = 0.6f;
        m.mass = 557.0f;
        m.dragCoefficient = 0.35f;
        m.liftCoefficient = 1.1f;
        m.maxHealth = 250.0f;        // Very fragile
        m.tailRotorHealth = 60.0f;
        m.engineHealth = 100.0f;
        m.fuelCapacity = 100.0f;
        m.fuelBurnRate = 0.06f;
        m.translationalLiftSpeed = 12.0f;
        m.vortexRingDescentRate = 8.0f;
        m.autorotateDescentRate = 10.0f;
        m.groundEffectHeight = 10.0f;
        m.groundEffectMultiplier = 1.25f;
        m_flightModels[static_cast<uint32_t>(HelicopterType::OH6_Loach)] = m;
        Logger::Debug("[HelicopterPhysics::InitializeFlightModels] registered OH6_Loach: maxSpeed=%.1f, mass=%.1f, maxHealth=%.1f",
                      m.maxSpeed, m.mass, m.maxHealth);
    }

    // AH-1G Cobra — attack helicopter
    {
        HeliFlightModel m;
        m.maxSpeed = 65.0f;          // ~125 knots
        m.maxClimbRate = 8.5f;
        m.maxDescentRate = 16.0f;
        m.cruiseSpeed = 50.0f;
        m.hoverStability = 0.7f;
        m.mainRotorRPM = 314.0f;
        m.rotorSpoolUpTime = 7.0f;
        m.rotorSpoolDownTime = 11.0f;
        m.pitchRate = 35.0f;
        m.rollRate = 45.0f;
        m.yawRate = 30.0f;
        m.collectiveRate = 0.55f;
        m.mass = 2754.0f;
        m.dragCoefficient = 0.38f;
        m.liftCoefficient = 1.05f;
        m.maxHealth = 600.0f;        // Toughest helicopter
        m.tailRotorHealth = 120.0f;
        m.engineHealth = 250.0f;
        m.fuelCapacity = 100.0f;
        m.fuelBurnRate = 0.1f;
        m.translationalLiftSpeed = 14.0f;
        m.vortexRingDescentRate = 11.0f;
        m.autorotateDescentRate = 13.0f;
        m.groundEffectHeight = 12.0f;
        m.groundEffectMultiplier = 1.2f;
        m_flightModels[static_cast<uint32_t>(HelicopterType::AH1_Cobra)] = m;
        Logger::Debug("[HelicopterPhysics::InitializeFlightModels] registered AH1_Cobra: maxSpeed=%.1f, mass=%.1f, maxHealth=%.1f",
                      m.maxSpeed, m.mass, m.maxHealth);
    }

    // ACH-47 Chinook — heavy transport
    {
        HeliFlightModel m;
        m.maxSpeed = 52.0f;
        m.maxClimbRate = 6.0f;
        m.maxDescentRate = 12.0f;
        m.cruiseSpeed = 40.0f;
        m.hoverStability = 0.9f;
        m.mainRotorRPM = 225.0f;
        m.rotorSpoolUpTime = 10.0f;
        m.rotorSpoolDownTime = 15.0f;
        m.pitchRate = 20.0f;
        m.rollRate = 25.0f;
        m.yawRate = 18.0f;
        m.collectiveRate = 0.4f;
        m.mass = 9736.0f;
        m.dragCoefficient = 0.5f;
        m.liftCoefficient = 1.2f;
        m.maxHealth = 800.0f;
        m.tailRotorHealth = 150.0f;
        m.engineHealth = 350.0f;
        m.fuelCapacity = 100.0f;
        m.fuelBurnRate = 0.15f;
        m.translationalLiftSpeed = 18.0f;
        m.vortexRingDescentRate = 12.0f;
        m.autorotateDescentRate = 15.0f;
        m.groundEffectHeight = 20.0f;
        m.groundEffectMultiplier = 1.35f;
        m_flightModels[static_cast<uint32_t>(HelicopterType::ACH47_Chinook)] = m;
        Logger::Debug("[HelicopterPhysics::InitializeFlightModels] registered ACH47_Chinook: maxSpeed=%.1f, mass=%.1f, maxHealth=%.1f",
                      m.maxSpeed, m.mass, m.maxHealth);
    }
    Logger::Info("[HelicopterPhysics::InitializeFlightModels] registered %zu flight models", m_flightModels.size());
    Logger::Trace("[HelicopterPhysics::InitializeFlightModels] exit");
}

void HelicopterPhysics::InitializeWeaponMounts(HelicopterState& heli) {
    Logger::Trace("[HelicopterPhysics::InitializeWeaponMounts] entry, heliId=%u, type=%d",
                  heli.vehicleId, static_cast<int>(heli.type));
    heli.weapons.clear();
    switch (heli.type) {
        case HelicopterType::UH1_Huey: {
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] configuring UH1_Huey weapons");
            // Left door M60D (seat 2)
            HeliWeaponMount leftDoor;
            leftDoor.type = HeliWeaponType::DoorGunM60;
            leftDoor.seatIndex = 2;
            leftDoor.ammo = 500; leftDoor.maxAmmo = 500;
            leftDoor.fireRate = 550.0f; leftDoor.damage = 80.0f;
            leftDoor.mountOffset = Vector3(-1.5f, 0.0f, -0.5f);
            leftDoor.traverseAngle = 90.0f;
            heli.weapons.push_back(leftDoor);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added left door M60D (seat 2, ammo=500)");

            // Right door M60D (seat 3)
            HeliWeaponMount rightDoor = leftDoor;
            rightDoor.seatIndex = 3;
            rightDoor.mountOffset = Vector3(1.5f, 0.0f, -0.5f);
            heli.weapons.push_back(rightDoor);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added right door M60D (seat 3, ammo=500)");
            break;
        }
        case HelicopterType::OH6_Loach: {
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] configuring OH6_Loach weapons");
            // M134 Minigun (pilot seat 0)
            HeliWeaponMount minigun;
            minigun.type = HeliWeaponType::Minigun;
            minigun.seatIndex = 0;
            minigun.ammo = 2000; minigun.maxAmmo = 2000;
            minigun.fireRate = 4000.0f; minigun.damage = 35.0f;
            minigun.mountOffset = Vector3(0.0f, 1.0f, -0.8f);
            minigun.traverseAngle = 15.0f;
            heli.weapons.push_back(minigun);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added M134 Minigun (seat 0, ammo=2000)");
            break;
        }
        case HelicopterType::AH1_Cobra: {
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] configuring AH1_Cobra weapons");
            // M195 20mm cannon (pilot)
            HeliWeaponMount cannon;
            cannon.type = HeliWeaponType::Minigun;
            cannon.seatIndex = 0;
            cannon.ammo = 750; cannon.maxAmmo = 750;
            cannon.fireRate = 750.0f; cannon.damage = 60.0f;
            cannon.mountOffset = Vector3(0.0f, 2.0f, -1.0f);
            cannon.traverseAngle = 10.0f;
            heli.weapons.push_back(cannon);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added M195 cannon (seat 0, ammo=750)");

            // M158 rocket pod (pilot)
            HeliWeaponMount rockets;
            rockets.type = HeliWeaponType::RocketPod;
            rockets.seatIndex = 0;
            rockets.ammo = 14; rockets.maxAmmo = 14;
            rockets.fireRate = 120.0f; rockets.damage = 200.0f;
            rockets.mountOffset = Vector3(0.0f, 1.5f, -0.3f);
            rockets.traverseAngle = 5.0f;
            heli.weapons.push_back(rockets);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added M158 rocket pod (seat 0, ammo=14)");

            // M134 Minigun (gunner seat 1)
            HeliWeaponMount gunnerMinigun;
            gunnerMinigun.type = HeliWeaponType::Minigun;
            gunnerMinigun.seatIndex = 1;
            gunnerMinigun.ammo = 4000; gunnerMinigun.maxAmmo = 4000;
            gunnerMinigun.fireRate = 4000.0f; gunnerMinigun.damage = 35.0f;
            gunnerMinigun.mountOffset = Vector3(0.0f, 2.5f, -1.2f);
            gunnerMinigun.traverseAngle = 110.0f;
            gunnerMinigun.elevationMin = -80.0f;
            gunnerMinigun.elevationMax = 15.0f;
            heli.weapons.push_back(gunnerMinigun);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added gunner M134 Minigun (seat 1, ammo=4000)");

            // M75 40mm grenade launcher (gunner)
            HeliWeaponMount grenades;
            grenades.type = HeliWeaponType::GrenadeLauncher40mm;
            grenades.seatIndex = 1;
            grenades.ammo = 150; grenades.maxAmmo = 150;
            grenades.fireRate = 220.0f; grenades.damage = 120.0f;
            grenades.mountOffset = Vector3(0.0f, 2.5f, -1.5f);
            grenades.traverseAngle = 110.0f;
            heli.weapons.push_back(grenades);
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] added M75 grenade launcher (seat 1, ammo=150)");
            break;
        }
        case HelicopterType::ACH47_Chinook:
            Logger::Debug("[HelicopterPhysics::InitializeWeaponMounts] ACH47_Chinook has no weapon mounts");
            // No standard weapon mounts for transport Chinook
            break;
    }
    Logger::Info("[HelicopterPhysics::InitializeWeaponMounts] initialized %zu weapon mounts for heli %u",
                 heli.weapons.size(), heli.vehicleId);
    Logger::Trace("[HelicopterPhysics::InitializeWeaponMounts] exit");
}

uint32_t HelicopterPhysics::SpawnHelicopter(HelicopterType type, uint32_t teamId, const Vector3& position) {
    Logger::Trace("[HelicopterPhysics::SpawnHelicopter] entry, type=%d, teamId=%u, pos=(%.1f, %.1f, %.1f)",
                  static_cast<int>(type), teamId, position.x, position.y, position.z);
    HelicopterState heli;
    heli.vehicleId = m_nextHeliId++;
    heli.type = type;
    heli.teamId = teamId;
    heli.position = position;
    heli.isLanded = true;

    auto modelIt = m_flightModels.find(static_cast<uint32_t>(type));
    if (modelIt != m_flightModels.end()) {
        heli.health = modelIt->second.maxHealth;
        heli.fuel = modelIt->second.fuelCapacity;
        Logger::Debug("[HelicopterPhysics::SpawnHelicopter] flight model found: health=%.1f, fuel=%.1f",
                      heli.health, heli.fuel);
    } else {
        Logger::Warn("[HelicopterPhysics::SpawnHelicopter] no flight model found for type=%d", static_cast<int>(type));
    }

    InitializeWeaponMounts(heli);

    uint32_t id = heli.vehicleId;
    m_helicopters[id] = std::move(heli);
    m_controlInputs[id] = {};

    Logger::Info("Helicopter spawned: id=%u, type=%d, team=%u at (%.1f, %.1f, %.1f)",
                 id, static_cast<int>(type), teamId, position.x, position.y, position.z);
    Logger::Trace("[HelicopterPhysics::SpawnHelicopter] exit, return id=%u", id);
    return id;
}

void HelicopterPhysics::DestroyHelicopter(uint32_t heliId) {
    Logger::Trace("[HelicopterPhysics::DestroyHelicopter] entry, heliId=%u", heliId);
    auto it = m_helicopters.find(heliId);
    if (it == m_helicopters.end()) {
        Logger::Warn("[HelicopterPhysics::DestroyHelicopter] helicopter %u not found", heliId);
        Logger::Trace("[HelicopterPhysics::DestroyHelicopter] exit (not found)");
        return;
    }

    it->second.health = 0;
    it->second.isOnFire = true;

    Logger::Info("Helicopter %u destroyed", heliId);
    Logger::Trace("[HelicopterPhysics::DestroyHelicopter] exit");
}

HelicopterState* HelicopterPhysics::GetHelicopter(uint32_t heliId) {
    Logger::Trace("[HelicopterPhysics::GetHelicopter] entry, heliId=%u", heliId);
    auto it = m_helicopters.find(heliId);
    HelicopterState* result = it != m_helicopters.end() ? &it->second : nullptr;
    Logger::Trace("[HelicopterPhysics::GetHelicopter] exit, return=%p", static_cast<void*>(result));
    return result;
}

const HelicopterState* HelicopterPhysics::GetHelicopter(uint32_t heliId) const {
    Logger::Trace("[HelicopterPhysics::GetHelicopter const] entry, heliId=%u", heliId);
    auto it = m_helicopters.find(heliId);
    const HelicopterState* result = it != m_helicopters.end() ? &it->second : nullptr;
    Logger::Trace("[HelicopterPhysics::GetHelicopter const] exit, return=%p", static_cast<const void*>(result));
    return result;
}

std::vector<const HelicopterState*> HelicopterPhysics::GetAllHelicopters() const {
    Logger::Trace("[HelicopterPhysics::GetAllHelicopters] entry");
    std::vector<const HelicopterState*> result;
    for (const auto& [id, h] : m_helicopters) result.push_back(&h);
    Logger::Trace("[HelicopterPhysics::GetAllHelicopters] exit, count=%zu", result.size());
    return result;
}

std::vector<const HelicopterState*> HelicopterPhysics::GetTeamHelicopters(uint32_t teamId) const {
    Logger::Trace("[HelicopterPhysics::GetTeamHelicopters] entry, teamId=%u", teamId);
    std::vector<const HelicopterState*> result;
    for (const auto& [id, h] : m_helicopters) {
        if (h.teamId == teamId) result.push_back(&h);
    }
    Logger::Trace("[HelicopterPhysics::GetTeamHelicopters] exit, count=%zu", result.size());
    return result;
}

void HelicopterPhysics::StartEngine(uint32_t heliId) {
    Logger::Trace("[HelicopterPhysics::StartEngine] entry, heliId=%u", heliId);
    auto* h = GetHelicopter(heliId);
    if (!h || h->engineDamage >= 1.0f || h->fuel <= 0.0f) {
        if (!h) {
            Logger::Warn("[HelicopterPhysics::StartEngine] helicopter %u not found", heliId);
        } else if (h->engineDamage >= 1.0f) {
            Logger::Warn("[HelicopterPhysics::StartEngine] helicopter %u engine too damaged (%.2f)", heliId, h->engineDamage);
        } else {
            Logger::Warn("[HelicopterPhysics::StartEngine] helicopter %u out of fuel (%.2f)", heliId, h->fuel);
        }
        Logger::Trace("[HelicopterPhysics::StartEngine] exit (cannot start)");
        return;
    }
    h->engineRunning = true;
    Logger::Info("Helicopter %u engine started", heliId);
    Logger::Trace("[HelicopterPhysics::StartEngine] exit");
}

void HelicopterPhysics::StopEngine(uint32_t heliId) {
    Logger::Trace("[HelicopterPhysics::StopEngine] entry, heliId=%u", heliId);
    auto* h = GetHelicopter(heliId);
    if (!h) {
        Logger::Warn("[HelicopterPhysics::StopEngine] helicopter %u not found", heliId);
        Logger::Trace("[HelicopterPhysics::StopEngine] exit (not found)");
        return;
    }
    h->engineRunning = false;
    Logger::Info("Helicopter %u engine stopped", heliId);
    Logger::Trace("[HelicopterPhysics::StopEngine] exit");
}

bool HelicopterPhysics::IsEngineRunning(uint32_t heliId) const {
    Logger::Trace("[HelicopterPhysics::IsEngineRunning] entry, heliId=%u", heliId);
    auto* h = GetHelicopter(heliId);
    bool result = h && h->engineRunning;
    Logger::Trace("[HelicopterPhysics::IsEngineRunning] exit, return=%s", result ? "true" : "false");
    return result;
}

void HelicopterPhysics::SetControlInput(uint32_t heliId, const HeliControlInput& input) {
    Logger::Trace("[HelicopterPhysics::SetControlInput] entry, heliId=%u, cyclic_pitch=%.2f, cyclic_roll=%.2f, collective=%.2f, pedal=%.2f",
                  heliId, input.cyclic_pitch, input.cyclic_roll, input.collective, input.pedal);
    m_controlInputs[heliId] = input;
    Logger::Trace("[HelicopterPhysics::SetControlInput] exit");
}

bool HelicopterPhysics::EnterHelicopter(uint32_t playerId, uint32_t heliId, uint32_t seatIndex) {
    Logger::Trace("[HelicopterPhysics::EnterHelicopter] entry, playerId=%u, heliId=%u, seatIndex=%u",
                  playerId, heliId, seatIndex);
    auto* h = GetHelicopter(heliId);
    if (!h || h->health <= 0) {
        Logger::Debug("[HelicopterPhysics::EnterHelicopter] cannot enter: heli=%p, health=%.1f",
                      static_cast<void*>(h), h ? h->health : 0.0f);
        Logger::Trace("[HelicopterPhysics::EnterHelicopter] exit, return=false (invalid heli or dead)");
        return false;
    }

    // Check if seat is already occupied
    if (seatIndex == 0 && h->pilotId != 0) {
        Logger::Debug("[HelicopterPhysics::EnterHelicopter] pilot seat already occupied by %u", h->pilotId);
        Logger::Trace("[HelicopterPhysics::EnterHelicopter] exit, return=false (seat occupied)");
        return false;
    }
    for (uint32_t occ : h->occupantIds) {
        if (occ == playerId) {
            Logger::Debug("[HelicopterPhysics::EnterHelicopter] player %u already in helicopter %u", playerId, heliId);
            Logger::Trace("[HelicopterPhysics::EnterHelicopter] exit, return=false (already in heli)");
            return false;
        }
    }

    if (seatIndex == 0) {
        h->pilotId = playerId;
        Logger::Debug("[HelicopterPhysics::EnterHelicopter] player %u assigned as pilot", playerId);
    } else {
        Logger::Debug("[HelicopterPhysics::EnterHelicopter] player %u assigned to seat %u", playerId, seatIndex);
    }
    h->occupantIds.push_back(playerId);

    Logger::Info("Player %u entered helicopter %u (seat %u)", playerId, heliId, seatIndex);
    Logger::Trace("[HelicopterPhysics::EnterHelicopter] exit, return=true");
    return true;
}

bool HelicopterPhysics::ExitHelicopter(uint32_t playerId) {
    Logger::Trace("[HelicopterPhysics::ExitHelicopter] entry, playerId=%u", playerId);
    for (auto& [id, heli] : m_helicopters) {
        if (heli.pilotId == playerId) {
            heli.pilotId = 0;
            Logger::Debug("[HelicopterPhysics::ExitHelicopter] player %u removed as pilot of heli %u", playerId, id);
        }
        auto it = std::find(heli.occupantIds.begin(), heli.occupantIds.end(), playerId);
        if (it != heli.occupantIds.end()) {
            heli.occupantIds.erase(it);
            Logger::Info("Player %u exited helicopter %u", playerId, id);
            Logger::Trace("[HelicopterPhysics::ExitHelicopter] exit, return=true");
            return true;
        }
    }
    Logger::Debug("[HelicopterPhysics::ExitHelicopter] player %u not found in any helicopter", playerId);
    Logger::Trace("[HelicopterPhysics::ExitHelicopter] exit, return=false");
    return false;
}

void HelicopterPhysics::ApplyDamage(uint32_t heliId, float damage, const Vector3& /*hitPoint*/) {
    Logger::Trace("[HelicopterPhysics::ApplyDamage] entry, heliId=%u, damage=%.1f", heliId, damage);
    auto* h = GetHelicopter(heliId);
    if (!h) {
        Logger::Warn("[HelicopterPhysics::ApplyDamage] helicopter %u not found", heliId);
        Logger::Trace("[HelicopterPhysics::ApplyDamage] exit (not found)");
        return;
    }
    float prevHealth = h->health;
    h->health -= damage;
    Logger::Debug("[HelicopterPhysics::ApplyDamage] heli %u health: %.1f -> %.1f (damage=%.1f)", heliId, prevHealth, h->health, damage);
    if (h->health <= 0) {
        h->health = 0;
        Logger::Debug("[HelicopterPhysics::ApplyDamage] heli %u health depleted, triggering destruction", heliId);
        DestroyHelicopter(heliId);
    }
    Logger::Trace("[HelicopterPhysics::ApplyDamage] exit");
}

void HelicopterPhysics::ApplyComponentDamage(uint32_t heliId, float damage, const std::string& component) {
    Logger::Trace("[HelicopterPhysics::ApplyComponentDamage] entry, heliId=%u, damage=%.1f, component=%s",
                  heliId, damage, component.c_str());
    auto* h = GetHelicopter(heliId);
    if (!h) {
        Logger::Warn("[HelicopterPhysics::ApplyComponentDamage] helicopter %u not found", heliId);
        Logger::Trace("[HelicopterPhysics::ApplyComponentDamage] exit (not found)");
        return;
    }

    if (component == "engine") {
        Logger::Debug("[HelicopterPhysics::ApplyComponentDamage] applying engine damage to heli %u", heliId);
        auto modelIt = m_flightModels.find(static_cast<uint32_t>(h->type));
        if (modelIt != m_flightModels.end()) {
            float prevDamage = h->engineDamage;
            h->engineDamage += damage / modelIt->second.engineHealth;
            h->engineDamage = std::min(h->engineDamage, 1.0f);
            Logger::Debug("[HelicopterPhysics::ApplyComponentDamage] engine damage: %.2f -> %.2f", prevDamage, h->engineDamage);
            if (h->engineDamage >= 1.0f) {
                h->engineRunning = false;
                Logger::Warn("Helicopter %u engine destroyed", heliId);
            }
        } else {
            Logger::Warn("[HelicopterPhysics::ApplyComponentDamage] no flight model for heli %u type=%d",
                         heliId, static_cast<int>(h->type));
        }
    } else if (component == "tailrotor") {
        Logger::Debug("[HelicopterPhysics::ApplyComponentDamage] applying tail rotor damage to heli %u", heliId);
        auto modelIt = m_flightModels.find(static_cast<uint32_t>(h->type));
        if (modelIt != m_flightModels.end()) {
            float prevDamage = h->tailRotorDamage;
            h->tailRotorDamage += damage / modelIt->second.tailRotorHealth;
            h->tailRotorDamage = std::min(h->tailRotorDamage, 1.0f);
            Logger::Debug("[HelicopterPhysics::ApplyComponentDamage] tail rotor damage: %.2f -> %.2f", prevDamage, h->tailRotorDamage);
            if (h->tailRotorDamage >= 1.0f) {
                Logger::Warn("Helicopter %u tail rotor destroyed — loss of yaw control", heliId);
            }
        } else {
            Logger::Warn("[HelicopterPhysics::ApplyComponentDamage] no flight model for heli %u type=%d",
                         heliId, static_cast<int>(h->type));
        }
    } else {
        Logger::Warn("[HelicopterPhysics::ApplyComponentDamage] unknown component '%s' for heli %u",
                     component.c_str(), heliId);
    }
    Logger::Trace("[HelicopterPhysics::ApplyComponentDamage] exit");
}

HeliFlightModel HelicopterPhysics::GetFlightModel(HelicopterType type) const {
    Logger::Trace("[HelicopterPhysics::GetFlightModel] entry, type=%d", static_cast<int>(type));
    auto it = m_flightModels.find(static_cast<uint32_t>(type));
    if (it == m_flightModels.end()) {
        Logger::Debug("[HelicopterPhysics::GetFlightModel] no flight model for type=%d, returning default", static_cast<int>(type));
    } else {
        Logger::Debug("[HelicopterPhysics::GetFlightModel] found flight model for type=%d", static_cast<int>(type));
    }
    HeliFlightModel result = it != m_flightModels.end() ? it->second : HeliFlightModel{};
    Logger::Trace("[HelicopterPhysics::GetFlightModel] exit");
    return result;
}

void HelicopterPhysics::Update(float deltaSeconds) {
    Logger::Trace("[HelicopterPhysics::Update] entry, deltaSeconds=%.4f, helicopterCount=%zu",
                  deltaSeconds, m_helicopters.size());
    for (auto& [id, heli] : m_helicopters) {
        if (heli.health <= 0) {
            Logger::Debug("[HelicopterPhysics::Update] heli %u dead, updating damage effects only", id);
            UpdateDamageEffects(heli, deltaSeconds);
            continue;
        }

        auto inputIt = m_controlInputs.find(id);
        HeliControlInput input = inputIt != m_controlInputs.end() ? inputIt->second : HeliControlInput{};

        auto modelIt = m_flightModels.find(static_cast<uint32_t>(heli.type));
        if (modelIt == m_flightModels.end()) {
            Logger::Warn("[HelicopterPhysics::Update] no flight model for heli %u type=%d, skipping",
                         id, static_cast<int>(heli.type));
            continue;
        }

        UpdateRotor(heli, modelIt->second, deltaSeconds);
        UpdateAerodynamics(heli, modelIt->second, input, deltaSeconds);
        UpdateGroundCollision(heli, deltaSeconds);
        UpdateDamageEffects(heli, deltaSeconds);
        UpdateWeapons(heli, input, deltaSeconds);
        CheckCrash(heli);

        // Fuel consumption
        if (heli.engineRunning) {
            float burnRate = modelIt->second.fuelBurnRate * (0.5f + 0.5f * std::abs(input.collective));
            heli.fuel -= burnRate * deltaSeconds;
            if (heli.fuel <= 0.0f) {
                heli.fuel = 0.0f;
                heli.engineRunning = false;
                Logger::Warn("Helicopter %u out of fuel", id);
            }
        }

        // Update derived state
        heli.airspeed = heli.velocity.Length();
        heli.altitude = heli.position.z - GetTerrainHeight(heli.position);
    }
    Logger::Trace("[HelicopterPhysics::Update] exit");
}

void HelicopterPhysics::UpdateRotor(HelicopterState& heli, const HeliFlightModel& model, float dt) {
    Logger::Trace("[HelicopterPhysics::UpdateRotor] entry, heliId=%u, engineRunning=%s, currentRPM=%.1f",
                  heli.vehicleId, heli.engineRunning ? "true" : "false", heli.currentRotorRPM);
    if (heli.engineRunning) {
        float targetRPM = model.mainRotorRPM * (1.0f - heli.engineDamage);
        float rpmDelta = (targetRPM - heli.currentRotorRPM) / model.rotorSpoolUpTime * dt;
        heli.currentRotorRPM = std::min(heli.currentRotorRPM + rpmDelta, targetRPM);
        Logger::Debug("[HelicopterPhysics::UpdateRotor] spooling up: targetRPM=%.1f, newRPM=%.1f", targetRPM, heli.currentRotorRPM);
    } else {
        float rpmDelta = heli.currentRotorRPM / model.rotorSpoolDownTime * dt;
        heli.currentRotorRPM = std::max(heli.currentRotorRPM - rpmDelta, 0.0f);
        Logger::Debug("[HelicopterPhysics::UpdateRotor] spooling down: newRPM=%.1f", heli.currentRotorRPM);
    }
    Logger::Trace("[HelicopterPhysics::UpdateRotor] exit");
}

void HelicopterPhysics::UpdateAerodynamics(HelicopterState& heli, const HeliFlightModel& model,
                                            const HeliControlInput& input, float dt) {
    Logger::Trace("[HelicopterPhysics::UpdateAerodynamics] entry, heliId=%u, dt=%.4f", heli.vehicleId, dt);
    // Only process flight physics if rotor is spinning
    float rpmRatio = heli.currentRotorRPM / model.mainRotorRPM;
    if (rpmRatio < 0.1f && !heli.isLanded) {
        // Freefalling with no rotor
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u freefalling (rpmRatio=%.2f)", heli.vehicleId, rpmRatio);
        heli.velocity.z -= GRAVITY * dt;
        heli.position += heli.velocity * dt;
        Logger::Trace("[HelicopterPhysics::UpdateAerodynamics] exit (freefall)");
        return;
    }

    // --- Control response ---
    // Pitch (forward/back tilt)
    float pitchInput = input.cyclic_pitch * model.pitchRate * dt;
    heli.pitch += pitchInput;
    heli.pitch = std::clamp(heli.pitch, -30.0f, 30.0f);

    // Roll (left/right tilt)
    float rollInput = input.cyclic_roll * model.rollRate * dt;
    heli.roll += rollInput;
    heli.roll = std::clamp(heli.roll, -45.0f, 45.0f);

    // Yaw (pedals)
    float yawRate = model.yawRate * (1.0f - heli.tailRotorDamage);
    heli.yaw += input.pedal * yawRate * dt;
    if (heli.yaw > 360.0f) heli.yaw -= 360.0f;
    if (heli.yaw < 0.0f) heli.yaw += 360.0f;

    // Tail rotor loss causes uncontrolled yaw spin
    if (heli.tailRotorDamage >= 1.0f) {
        heli.yaw += 120.0f * dt;  // Spin out of control
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u uncontrolled yaw spin (tail rotor destroyed)", heli.vehicleId);
    }

    // --- Force calculations ---
    Vector3 lift = CalculateLift(heli, model);
    Vector3 drag = CalculateDrag(heli, model);
    Vector3 gravity = CalculateGravity(model);

    // Convert pitch/roll to movement forces
    float pitchRad = heli.pitch * DEG_TO_RAD;
    float rollRad = heli.roll * DEG_TO_RAD;
    float yawRad = heli.yaw * DEG_TO_RAD;

    // Forward component from pitch
    float forwardForce = -std::sin(pitchRad) * lift.z * 0.8f;
    // Lateral component from roll
    float lateralForce = std::sin(rollRad) * lift.z * 0.6f;

    // Apply forces in world space (yaw-rotated)
    Vector3 accel;
    accel.x = (forwardForce * std::sin(yawRad) + lateralForce * std::cos(yawRad)) / model.mass;
    accel.y = (forwardForce * std::cos(yawRad) - lateralForce * std::sin(yawRad)) / model.mass;
    accel.z = (lift.z + gravity.z + drag.z) / model.mass;

    // Damping on horizontal velocity
    accel.x += drag.x / model.mass;
    accel.y += drag.y / model.mass;

    // Autorotation: if engine dead but rotor spinning, can still generate some lift
    if (!heli.engineRunning && rpmRatio > 0.3f) {
        heli.isAutorotating = true;
        float autoLift = GRAVITY * model.mass * 0.6f * rpmRatio;
        accel.z += autoLift / model.mass;
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u autorotating (rpmRatio=%.2f, autoLift=%.1f)",
                      heli.vehicleId, rpmRatio, autoLift);
    } else {
        heli.isAutorotating = false;
    }

    // Vortex ring state detection
    heli.inVortexRing = (heli.velocity.z < -model.vortexRingDescentRate && rpmRatio > 0.5f &&
                         heli.airspeed < model.translationalLiftSpeed);

    if (heli.inVortexRing) {
        accel.z -= 3.0f;  // Additional descent
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u in vortex ring state", heli.vehicleId);
    }

    // Translational lift
    float horizontalSpeed = std::sqrt(heli.velocity.x * heli.velocity.x + heli.velocity.y * heli.velocity.y);
    heli.inTranslationalLift = (horizontalSpeed > model.translationalLiftSpeed);
    if (heli.inTranslationalLift) {
        accel.z += 1.5f;  // Bonus lift from forward speed
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u in translational lift (hSpeed=%.1f)", heli.vehicleId, horizontalSpeed);
    }

    // Ground effect
    float terrainH = GetTerrainHeight(heli.position);
    float agl = heli.position.z - terrainH;
    heli.inGroundEffect = (agl < model.groundEffectHeight && agl > 0.0f);
    if (heli.inGroundEffect) {
        float geFactor = 1.0f + (model.groundEffectMultiplier - 1.0f) * (1.0f - agl / model.groundEffectHeight);
        accel.z *= geFactor;
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u in ground effect (agl=%.1f, geFactor=%.2f)",
                      heli.vehicleId, agl, geFactor);
    }

    // Integrate
    heli.velocity += accel * dt;

    // Clamp speeds
    float hSpeed = std::sqrt(heli.velocity.x * heli.velocity.x + heli.velocity.y * heli.velocity.y);
    if (hSpeed > model.maxSpeed) {
        float scale = model.maxSpeed / hSpeed;
        heli.velocity.x *= scale;
        heli.velocity.y *= scale;
        Logger::Debug("[HelicopterPhysics::UpdateAerodynamics] heli %u speed clamped from %.1f to %.1f",
                      heli.vehicleId, hSpeed, model.maxSpeed);
    }
    heli.velocity.z = std::clamp(heli.velocity.z, -model.maxDescentRate, model.maxClimbRate);

    heli.position += heli.velocity * dt;

    // Natural centering of cyclic when no input
    if (std::abs(input.cyclic_pitch) < 0.05f) heli.pitch *= (1.0f - 2.0f * dt);
    if (std::abs(input.cyclic_roll) < 0.05f) heli.roll *= (1.0f - 2.0f * dt);
    Logger::Trace("[HelicopterPhysics::UpdateAerodynamics] exit");
}

void HelicopterPhysics::UpdateGroundCollision(HelicopterState& heli, float /*dt*/) {
    Logger::Trace("[HelicopterPhysics::UpdateGroundCollision] entry, heliId=%u", heli.vehicleId);
    float terrainH = GetTerrainHeight(heli.position);
    if (heli.position.z <= terrainH) {
        heli.position.z = terrainH;
        heli.isLanded = true;
        Logger::Debug("[HelicopterPhysics::UpdateGroundCollision] heli %u on ground (terrainH=%.1f)", heli.vehicleId, terrainH);

        // Hard landing check
        if (heli.velocity.z < -5.0f) {
            float impactDmg = std::abs(heli.velocity.z) * 10.0f;
            heli.health -= impactDmg;
            Logger::Debug("Helicopter hard landing: impact damage=%.1f, health=%.1f",
                         impactDmg, heli.health);
        }

        heli.velocity.z = 0.0f;
        // Ground friction
        heli.velocity.x *= 0.95f;
        heli.velocity.y *= 0.95f;
    } else {
        heli.isLanded = false;
        Logger::Debug("[HelicopterPhysics::UpdateGroundCollision] heli %u airborne (agl=%.1f)", heli.vehicleId, heli.position.z - terrainH);
    }
    Logger::Trace("[HelicopterPhysics::UpdateGroundCollision] exit");
}

void HelicopterPhysics::UpdateDamageEffects(HelicopterState& heli, float dt) {
    Logger::Trace("[HelicopterPhysics::UpdateDamageEffects] entry, heliId=%u, isOnFire=%s",
                  heli.vehicleId, heli.isOnFire ? "true" : "false");
    if (heli.isOnFire) {
        heli.fireTimer += dt;
        heli.health -= 20.0f * dt;  // Burn damage
        Logger::Debug("[HelicopterPhysics::UpdateDamageEffects] heli %u burning (fireTimer=%.1f, health=%.1f)",
                      heli.vehicleId, heli.fireTimer, heli.health);
        if (heli.health <= -200.0f) {
            // Explosion — remove wreck after enough burn
            Logger::Info("Helicopter %u exploded after fire", heli.vehicleId);
        }
    }
    Logger::Trace("[HelicopterPhysics::UpdateDamageEffects] exit");
}

void HelicopterPhysics::UpdateWeapons(HelicopterState& heli, const HeliControlInput& input, float dt) {
    Logger::Trace("[HelicopterPhysics::UpdateWeapons] entry, heliId=%u, weaponCount=%zu, fireWeapon=%s",
                  heli.vehicleId, heli.weapons.size(), input.fireWeapon ? "true" : "false");
    for (auto& weapon : heli.weapons) {
        weapon.timeSinceLastShot += dt;

        if (input.fireWeapon && input.weaponIndex < heli.weapons.size() &&
            &weapon == &heli.weapons[input.weaponIndex]) {
            float fireInterval = 60.0f / weapon.fireRate;
            if (weapon.timeSinceLastShot >= fireInterval && weapon.ammo > 0) {
                weapon.ammo--;
                weapon.timeSinceLastShot = 0.0f;
                Logger::Debug("[HelicopterPhysics::UpdateWeapons] heli %u weapon[%u] fired, ammo=%d/%d",
                              heli.vehicleId, input.weaponIndex, weapon.ammo, weapon.maxAmmo);
                // Fire event would be processed by DamageSystem
            } else if (weapon.ammo <= 0) {
                Logger::Debug("[HelicopterPhysics::UpdateWeapons] heli %u weapon[%u] out of ammo", heli.vehicleId, input.weaponIndex);
            }
        }
    }
    Logger::Trace("[HelicopterPhysics::UpdateWeapons] exit");
}

void HelicopterPhysics::CheckCrash(HelicopterState& heli) {
    Logger::Trace("[HelicopterPhysics::CheckCrash] entry, heliId=%u, health=%.1f, isOnFire=%s",
                  heli.vehicleId, heli.health, heli.isOnFire ? "true" : "false");
    if (heli.health <= 0 && !heli.isOnFire) {
        heli.isOnFire = true;
        heli.fireTimer = 0.0f;
        Logger::Info("Helicopter %u crashed", heli.vehicleId);
    } else {
        Logger::Debug("[HelicopterPhysics::CheckCrash] heli %u no crash (health=%.1f, onFire=%s)",
                      heli.vehicleId, heli.health, heli.isOnFire ? "true" : "false");
    }
    Logger::Trace("[HelicopterPhysics::CheckCrash] exit");
}

float HelicopterPhysics::GetTerrainHeight(const Vector3& /*position*/) const {
    Logger::Trace("[HelicopterPhysics::GetTerrainHeight] entry");
    // Terrain query would come from the map system
    Logger::Trace("[HelicopterPhysics::GetTerrainHeight] exit, return=0.0");
    return 0.0f;
}

Vector3 HelicopterPhysics::CalculateLift(const HelicopterState& heli, const HeliFlightModel& model) const {
    Logger::Trace("[HelicopterPhysics::CalculateLift] entry, heliId=%u, rpm=%.1f, collective=%.2f",
                  heli.vehicleId, heli.currentRotorRPM, heli.collectivePower);
    float rpmRatio = heli.currentRotorRPM / model.mainRotorRPM;
    float collectivePower = heli.collectivePower;

    // Lift = rpm_ratio * collective * lift_coefficient * weight
    float liftForce = rpmRatio * collectivePower * model.liftCoefficient * model.mass * GRAVITY;

    // Engine damage reduces lift
    liftForce *= (1.0f - heli.engineDamage * 0.8f);

    Logger::Debug("[HelicopterPhysics::CalculateLift] rpmRatio=%.2f, liftForce=%.1f", rpmRatio, liftForce);
    Logger::Trace("[HelicopterPhysics::CalculateLift] exit, return=(0, 0, %.1f)", liftForce);
    return Vector3(0, 0, liftForce);
}

Vector3 HelicopterPhysics::CalculateDrag(const HelicopterState& heli, const HeliFlightModel& model) const {
    Logger::Trace("[HelicopterPhysics::CalculateDrag] entry, heliId=%u", heli.vehicleId);
    float speedSq = heli.velocity.LengthSquared();
    if (speedSq < 0.01f) {
        Logger::Trace("[HelicopterPhysics::CalculateDrag] exit, return=Zero (speed too low)");
        return Vector3::Zero();
    }

    Vector3 dragDir = heli.velocity.Normalized() * (-1.0f);
    float dragMag = 0.5f * 1.225f * model.dragCoefficient * speedSq;  // rho * Cd * v^2

    Logger::Debug("[HelicopterPhysics::CalculateDrag] speedSq=%.2f, dragMag=%.2f", speedSq, dragMag);
    Vector3 result = dragDir * dragMag;
    Logger::Trace("[HelicopterPhysics::CalculateDrag] exit, return=(%.1f, %.1f, %.1f)", result.x, result.y, result.z);
    return result;
}

Vector3 HelicopterPhysics::CalculateGravity(const HeliFlightModel& model) const {
    Logger::Trace("[HelicopterPhysics::CalculateGravity] entry, mass=%.1f", model.mass);
    Vector3 result(0, 0, -GRAVITY * model.mass);
    Logger::Trace("[HelicopterPhysics::CalculateGravity] exit, return=(0, 0, %.1f)", result.z);
    return result;
}
