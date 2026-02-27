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
}

HelicopterPhysics::~HelicopterPhysics() {
    Shutdown();
}

void HelicopterPhysics::Initialize() {
    m_helicopters.clear();
    m_controlInputs.clear();
    m_flightModels.clear();
    InitializeFlightModels();
    Logger::Info("HelicopterPhysics initialized");
}

void HelicopterPhysics::Shutdown() {
    m_helicopters.clear();
    m_controlInputs.clear();
}

void HelicopterPhysics::InitializeFlightModels() {
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
    }
}

void HelicopterPhysics::InitializeWeaponMounts(HelicopterState& heli) {
    heli.weapons.clear();
    switch (heli.type) {
        case HelicopterType::UH1_Huey: {
            // Left door M60D (seat 2)
            HeliWeaponMount leftDoor;
            leftDoor.type = HeliWeaponType::DoorGunM60;
            leftDoor.seatIndex = 2;
            leftDoor.ammo = 500; leftDoor.maxAmmo = 500;
            leftDoor.fireRate = 550.0f; leftDoor.damage = 80.0f;
            leftDoor.mountOffset = Vector3(-1.5f, 0.0f, -0.5f);
            leftDoor.traverseAngle = 90.0f;
            heli.weapons.push_back(leftDoor);

            // Right door M60D (seat 3)
            HeliWeaponMount rightDoor = leftDoor;
            rightDoor.seatIndex = 3;
            rightDoor.mountOffset = Vector3(1.5f, 0.0f, -0.5f);
            heli.weapons.push_back(rightDoor);
            break;
        }
        case HelicopterType::OH6_Loach: {
            // M134 Minigun (pilot seat 0)
            HeliWeaponMount minigun;
            minigun.type = HeliWeaponType::Minigun;
            minigun.seatIndex = 0;
            minigun.ammo = 2000; minigun.maxAmmo = 2000;
            minigun.fireRate = 4000.0f; minigun.damage = 35.0f;
            minigun.mountOffset = Vector3(0.0f, 1.0f, -0.8f);
            minigun.traverseAngle = 15.0f;
            heli.weapons.push_back(minigun);
            break;
        }
        case HelicopterType::AH1_Cobra: {
            // M195 20mm cannon (pilot)
            HeliWeaponMount cannon;
            cannon.type = HeliWeaponType::Minigun;
            cannon.seatIndex = 0;
            cannon.ammo = 750; cannon.maxAmmo = 750;
            cannon.fireRate = 750.0f; cannon.damage = 60.0f;
            cannon.mountOffset = Vector3(0.0f, 2.0f, -1.0f);
            cannon.traverseAngle = 10.0f;
            heli.weapons.push_back(cannon);

            // M158 rocket pod (pilot)
            HeliWeaponMount rockets;
            rockets.type = HeliWeaponType::RocketPod;
            rockets.seatIndex = 0;
            rockets.ammo = 14; rockets.maxAmmo = 14;
            rockets.fireRate = 120.0f; rockets.damage = 200.0f;
            rockets.mountOffset = Vector3(0.0f, 1.5f, -0.3f);
            rockets.traverseAngle = 5.0f;
            heli.weapons.push_back(rockets);

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

            // M75 40mm grenade launcher (gunner)
            HeliWeaponMount grenades;
            grenades.type = HeliWeaponType::GrenadeLauncher40mm;
            grenades.seatIndex = 1;
            grenades.ammo = 150; grenades.maxAmmo = 150;
            grenades.fireRate = 220.0f; grenades.damage = 120.0f;
            grenades.mountOffset = Vector3(0.0f, 2.5f, -1.5f);
            grenades.traverseAngle = 110.0f;
            heli.weapons.push_back(grenades);
            break;
        }
        case HelicopterType::ACH47_Chinook:
            // No standard weapon mounts for transport Chinook
            break;
    }
}

uint32_t HelicopterPhysics::SpawnHelicopter(HelicopterType type, uint32_t teamId, const Vector3& position) {
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
    }

    InitializeWeaponMounts(heli);

    uint32_t id = heli.vehicleId;
    m_helicopters[id] = std::move(heli);
    m_controlInputs[id] = {};

    Logger::Info("Helicopter spawned: id=%u, type=%d, team=%u at (%.1f, %.1f, %.1f)",
                 id, static_cast<int>(type), teamId, position.x, position.y, position.z);
    return id;
}

void HelicopterPhysics::DestroyHelicopter(uint32_t heliId) {
    auto it = m_helicopters.find(heliId);
    if (it == m_helicopters.end()) return;

    it->second.health = 0;
    it->second.isOnFire = true;

    Logger::Info("Helicopter %u destroyed", heliId);
}

HelicopterState* HelicopterPhysics::GetHelicopter(uint32_t heliId) {
    auto it = m_helicopters.find(heliId);
    return it != m_helicopters.end() ? &it->second : nullptr;
}

const HelicopterState* HelicopterPhysics::GetHelicopter(uint32_t heliId) const {
    auto it = m_helicopters.find(heliId);
    return it != m_helicopters.end() ? &it->second : nullptr;
}

std::vector<const HelicopterState*> HelicopterPhysics::GetAllHelicopters() const {
    std::vector<const HelicopterState*> result;
    for (const auto& [id, h] : m_helicopters) result.push_back(&h);
    return result;
}

std::vector<const HelicopterState*> HelicopterPhysics::GetTeamHelicopters(uint32_t teamId) const {
    std::vector<const HelicopterState*> result;
    for (const auto& [id, h] : m_helicopters) {
        if (h.teamId == teamId) result.push_back(&h);
    }
    return result;
}

void HelicopterPhysics::StartEngine(uint32_t heliId) {
    auto* h = GetHelicopter(heliId);
    if (!h || h->engineDamage >= 1.0f || h->fuel <= 0.0f) return;
    h->engineRunning = true;
    Logger::Info("Helicopter %u engine started", heliId);
}

void HelicopterPhysics::StopEngine(uint32_t heliId) {
    auto* h = GetHelicopter(heliId);
    if (!h) return;
    h->engineRunning = false;
    Logger::Info("Helicopter %u engine stopped", heliId);
}

bool HelicopterPhysics::IsEngineRunning(uint32_t heliId) const {
    auto* h = GetHelicopter(heliId);
    return h && h->engineRunning;
}

void HelicopterPhysics::SetControlInput(uint32_t heliId, const HeliControlInput& input) {
    m_controlInputs[heliId] = input;
}

bool HelicopterPhysics::EnterHelicopter(uint32_t playerId, uint32_t heliId, uint32_t seatIndex) {
    auto* h = GetHelicopter(heliId);
    if (!h || h->health <= 0) return false;

    // Check if seat is already occupied
    if (seatIndex == 0 && h->pilotId != 0) return false;
    for (uint32_t occ : h->occupantIds) {
        if (occ == playerId) return false;
    }

    if (seatIndex == 0) {
        h->pilotId = playerId;
    }
    h->occupantIds.push_back(playerId);

    Logger::Info("Player %u entered helicopter %u (seat %u)", playerId, heliId, seatIndex);
    return true;
}

bool HelicopterPhysics::ExitHelicopter(uint32_t playerId) {
    for (auto& [id, heli] : m_helicopters) {
        if (heli.pilotId == playerId) heli.pilotId = 0;
        auto it = std::find(heli.occupantIds.begin(), heli.occupantIds.end(), playerId);
        if (it != heli.occupantIds.end()) {
            heli.occupantIds.erase(it);
            Logger::Info("Player %u exited helicopter %u", playerId, id);
            return true;
        }
    }
    return false;
}

void HelicopterPhysics::ApplyDamage(uint32_t heliId, float damage, const Vector3& /*hitPoint*/) {
    auto* h = GetHelicopter(heliId);
    if (!h) return;
    h->health -= damage;
    if (h->health <= 0) {
        h->health = 0;
        DestroyHelicopter(heliId);
    }
}

void HelicopterPhysics::ApplyComponentDamage(uint32_t heliId, float damage, const std::string& component) {
    auto* h = GetHelicopter(heliId);
    if (!h) return;

    if (component == "engine") {
        auto modelIt = m_flightModels.find(static_cast<uint32_t>(h->type));
        if (modelIt != m_flightModels.end()) {
            h->engineDamage += damage / modelIt->second.engineHealth;
            h->engineDamage = std::min(h->engineDamage, 1.0f);
            if (h->engineDamage >= 1.0f) {
                h->engineRunning = false;
                Logger::Warn("Helicopter %u engine destroyed", heliId);
            }
        }
    } else if (component == "tailrotor") {
        auto modelIt = m_flightModels.find(static_cast<uint32_t>(h->type));
        if (modelIt != m_flightModels.end()) {
            h->tailRotorDamage += damage / modelIt->second.tailRotorHealth;
            h->tailRotorDamage = std::min(h->tailRotorDamage, 1.0f);
            if (h->tailRotorDamage >= 1.0f) {
                Logger::Warn("Helicopter %u tail rotor destroyed — loss of yaw control", heliId);
            }
        }
    }
}

HeliFlightModel HelicopterPhysics::GetFlightModel(HelicopterType type) const {
    auto it = m_flightModels.find(static_cast<uint32_t>(type));
    return it != m_flightModels.end() ? it->second : HeliFlightModel{};
}

void HelicopterPhysics::Update(float deltaSeconds) {
    for (auto& [id, heli] : m_helicopters) {
        if (heli.health <= 0) {
            UpdateDamageEffects(heli, deltaSeconds);
            continue;
        }

        auto inputIt = m_controlInputs.find(id);
        HeliControlInput input = inputIt != m_controlInputs.end() ? inputIt->second : HeliControlInput{};

        auto modelIt = m_flightModels.find(static_cast<uint32_t>(heli.type));
        if (modelIt == m_flightModels.end()) continue;

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
}

void HelicopterPhysics::UpdateRotor(HelicopterState& heli, const HeliFlightModel& model, float dt) {
    if (heli.engineRunning) {
        float targetRPM = model.mainRotorRPM * (1.0f - heli.engineDamage);
        float rpmDelta = (targetRPM - heli.currentRotorRPM) / model.rotorSpoolUpTime * dt;
        heli.currentRotorRPM = std::min(heli.currentRotorRPM + rpmDelta, targetRPM);
    } else {
        float rpmDelta = heli.currentRotorRPM / model.rotorSpoolDownTime * dt;
        heli.currentRotorRPM = std::max(heli.currentRotorRPM - rpmDelta, 0.0f);
    }
}

void HelicopterPhysics::UpdateAerodynamics(HelicopterState& heli, const HeliFlightModel& model,
                                            const HeliControlInput& input, float dt) {
    // Only process flight physics if rotor is spinning
    float rpmRatio = heli.currentRotorRPM / model.mainRotorRPM;
    if (rpmRatio < 0.1f && !heli.isLanded) {
        // Freefalling with no rotor
        heli.velocity.z -= GRAVITY * dt;
        heli.position += heli.velocity * dt;
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
    } else {
        heli.isAutorotating = false;
    }

    // Vortex ring state detection
    heli.inVortexRing = (heli.velocity.z < -model.vortexRingDescentRate && rpmRatio > 0.5f &&
                         heli.airspeed < model.translationalLiftSpeed);

    if (heli.inVortexRing) {
        accel.z -= 3.0f;  // Additional descent
    }

    // Translational lift
    float horizontalSpeed = std::sqrt(heli.velocity.x * heli.velocity.x + heli.velocity.y * heli.velocity.y);
    heli.inTranslationalLift = (horizontalSpeed > model.translationalLiftSpeed);
    if (heli.inTranslationalLift) {
        accel.z += 1.5f;  // Bonus lift from forward speed
    }

    // Ground effect
    float terrainH = GetTerrainHeight(heli.position);
    float agl = heli.position.z - terrainH;
    heli.inGroundEffect = (agl < model.groundEffectHeight && agl > 0.0f);
    if (heli.inGroundEffect) {
        float geFactor = 1.0f + (model.groundEffectMultiplier - 1.0f) * (1.0f - agl / model.groundEffectHeight);
        accel.z *= geFactor;
    }

    // Integrate
    heli.velocity += accel * dt;

    // Clamp speeds
    float hSpeed = std::sqrt(heli.velocity.x * heli.velocity.x + heli.velocity.y * heli.velocity.y);
    if (hSpeed > model.maxSpeed) {
        float scale = model.maxSpeed / hSpeed;
        heli.velocity.x *= scale;
        heli.velocity.y *= scale;
    }
    heli.velocity.z = std::clamp(heli.velocity.z, -model.maxDescentRate, model.maxClimbRate);

    heli.position += heli.velocity * dt;

    // Natural centering of cyclic when no input
    if (std::abs(input.cyclic_pitch) < 0.05f) heli.pitch *= (1.0f - 2.0f * dt);
    if (std::abs(input.cyclic_roll) < 0.05f) heli.roll *= (1.0f - 2.0f * dt);
}

void HelicopterPhysics::UpdateGroundCollision(HelicopterState& heli, float /*dt*/) {
    float terrainH = GetTerrainHeight(heli.position);
    if (heli.position.z <= terrainH) {
        heli.position.z = terrainH;
        heli.isLanded = true;

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
    }
}

void HelicopterPhysics::UpdateDamageEffects(HelicopterState& heli, float dt) {
    if (heli.isOnFire) {
        heli.fireTimer += dt;
        heli.health -= 20.0f * dt;  // Burn damage
        if (heli.health <= -200.0f) {
            // Explosion — remove wreck after enough burn
            Logger::Info("Helicopter %u exploded after fire", heli.vehicleId);
        }
    }
}

void HelicopterPhysics::UpdateWeapons(HelicopterState& heli, const HeliControlInput& input, float dt) {
    for (auto& weapon : heli.weapons) {
        weapon.timeSinceLastShot += dt;

        if (input.fireWeapon && input.weaponIndex < heli.weapons.size() &&
            &weapon == &heli.weapons[input.weaponIndex]) {
            float fireInterval = 60.0f / weapon.fireRate;
            if (weapon.timeSinceLastShot >= fireInterval && weapon.ammo > 0) {
                weapon.ammo--;
                weapon.timeSinceLastShot = 0.0f;
                // Fire event would be processed by DamageSystem
            }
        }
    }
}

void HelicopterPhysics::CheckCrash(HelicopterState& heli) {
    if (heli.health <= 0 && !heli.isOnFire) {
        heli.isOnFire = true;
        heli.fireTimer = 0.0f;
        Logger::Info("Helicopter %u crashed", heli.vehicleId);
    }
}

float HelicopterPhysics::GetTerrainHeight(const Vector3& /*position*/) const {
    // Terrain query would come from the map system
    return 0.0f;
}

Vector3 HelicopterPhysics::CalculateLift(const HelicopterState& heli, const HeliFlightModel& model) const {
    float rpmRatio = heli.currentRotorRPM / model.mainRotorRPM;
    float collectivePower = heli.collectivePower;

    // Lift = rpm_ratio * collective * lift_coefficient * weight
    float liftForce = rpmRatio * collectivePower * model.liftCoefficient * model.mass * GRAVITY;

    // Engine damage reduces lift
    liftForce *= (1.0f - heli.engineDamage * 0.8f);

    return Vector3(0, 0, liftForce);
}

Vector3 HelicopterPhysics::CalculateDrag(const HelicopterState& heli, const HeliFlightModel& model) const {
    float speedSq = heli.velocity.LengthSquared();
    if (speedSq < 0.01f) return Vector3::Zero();

    Vector3 dragDir = heli.velocity.Normalized() * (-1.0f);
    float dragMag = 0.5f * 1.225f * model.dragCoefficient * speedSq;  // rho * Cd * v^2

    return dragDir * dragMag;
}

Vector3 HelicopterPhysics::CalculateGravity(const HeliFlightModel& model) const {
    return Vector3(0, 0, -GRAVITY * model.mass);
}
