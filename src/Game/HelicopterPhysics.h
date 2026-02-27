// src/Game/HelicopterPhysics.h
// RS2V helicopter flight model — UH-1 Huey, OH-6 Loach, AH-1 Cobra

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "Math/Vector3.h"

class GameServer;

// Helicopter types in RS2V
enum class HelicopterType : uint8_t {
    UH1_Huey,          // UH-1D/H Iroquois — transport (pilot + co-pilot + 2 door gunners + passengers)
    OH6_Loach,          // OH-6A Cayuse — light observation (pilot + observer/gunner)
    AH1_Cobra,          // AH-1G Cobra — attack helicopter (pilot + gunner)
    ACH47_Chinook       // Transport — heavy lift
};

// Helicopter weapon mount types
enum class HeliWeaponType : uint8_t {
    DoorGunM60,         // M60D door gun (UH-1 door gunners)
    Minigun,            // M134 Minigun (AH-1, OH-6)
    RocketPod,          // M158 7-tube 2.75" rocket pod (AH-1)
    GrenadeLauncher40mm // M75 grenade launcher (AH-1 nose turret)
};

struct HeliWeaponMount {
    HeliWeaponType type;
    uint32_t seatIndex;             // Which seat controls this weapon
    float ammo = 0;
    float maxAmmo = 0;
    float fireRate = 600.0f;        // RPM
    float damage = 35.0f;
    float timeSinceLastShot = 0.0f;
    Vector3 mountOffset;            // Position relative to helicopter
    float traverseAngle = 60.0f;    // degrees of horizontal arc
    float elevationMin = -60.0f;    // degrees
    float elevationMax = 10.0f;     // degrees
};

// Helicopter flight characteristics
struct HeliFlightModel {
    // Performance
    float maxSpeed = 60.0f;             // m/s (~120 knots for Huey)
    float maxClimbRate = 8.0f;          // m/s
    float maxDescentRate = 15.0f;       // m/s
    float cruiseSpeed = 45.0f;          // m/s
    float hoverStability = 0.8f;        // 0-1, how stable in hover

    // Rotor physics
    float mainRotorRPM = 324.0f;        // Normal operating RPM
    float rotorSpoolUpTime = 8.0f;      // Seconds to reach operating RPM
    float rotorSpoolDownTime = 12.0f;   // Seconds to stop

    // Control responsiveness
    float pitchRate = 30.0f;            // degrees/s
    float rollRate = 40.0f;             // degrees/s
    float yawRate = 25.0f;              // degrees/s
    float collectiveRate = 0.5f;        // per second (0-1 range)

    // Physics
    float mass = 2200.0f;              // kg (UH-1 empty weight)
    float dragCoefficient = 0.4f;
    float liftCoefficient = 1.0f;

    // Damage model
    float maxHealth = 500.0f;
    float tailRotorHealth = 100.0f;
    float engineHealth = 200.0f;
    float fuelCapacity = 100.0f;        // percent
    float fuelBurnRate = 0.1f;          // percent per second at cruise

    // DCS-lite flight model parameters
    float translationalLiftSpeed = 15.0f;   // m/s — ETL speed
    float vortexRingDescentRate = 10.0f;     // m/s — vortex ring state threshold
    float autorotateDescentRate = 12.0f;     // m/s — autorotation descent
    float groundEffectHeight = 15.0f;        // meters — ground effect ceiling
    float groundEffectMultiplier = 1.3f;     // lift bonus in ground effect
};

// Current helicopter state
struct HelicopterState {
    uint32_t vehicleId = 0;
    HelicopterType type = HelicopterType::UH1_Huey;
    uint32_t teamId = 0;

    // Position and orientation (Euler angles in degrees)
    Vector3 position;
    Vector3 velocity;
    float pitch = 0.0f;        // nose up/down
    float roll = 0.0f;         // bank angle
    float yaw = 0.0f;          // heading

    // Engine state
    bool engineRunning = false;
    float engineDamage = 0.0f;      // 0-1
    float tailRotorDamage = 0.0f;   // 0-1
    float fuel = 100.0f;

    // Rotor state (runtime, not from flight model)
    float currentRotorRPM = 0.0f;
    float collectivePower = 0.0f;   // 0-1 current collective input

    // Flight state
    float altitude = 0.0f;
    float airspeed = 0.0f;
    bool isLanded = true;
    bool isAutorotating = false;
    bool inGroundEffect = false;
    bool inVortexRing = false;
    bool inTranslationalLift = false;

    // Damage
    float health = 500.0f;
    bool isOnFire = false;
    float fireTimer = 0.0f;

    // Occupants
    uint32_t pilotId = 0;
    std::vector<uint32_t> occupantIds;

    // Weapons
    std::vector<HeliWeaponMount> weapons;
};

// Control input from pilot
struct HeliControlInput {
    float collective = 0.0f;    // -1 to 1 (down/up)
    float cyclic_pitch = 0.0f;  // -1 to 1 (back/forward)
    float cyclic_roll = 0.0f;   // -1 to 1 (left/right)
    float pedal = 0.0f;         // -1 to 1 (yaw left/right)
    bool fireWeapon = false;
    uint32_t weaponIndex = 0;
};

class HelicopterPhysics {
public:
    explicit HelicopterPhysics(GameServer* server);
    ~HelicopterPhysics();

    void Initialize();
    void Shutdown();

    // Helicopter lifecycle
    uint32_t SpawnHelicopter(HelicopterType type, uint32_t teamId, const Vector3& position);
    void DestroyHelicopter(uint32_t heliId);
    HelicopterState* GetHelicopter(uint32_t heliId);
    const HelicopterState* GetHelicopter(uint32_t heliId) const;
    std::vector<const HelicopterState*> GetAllHelicopters() const;
    std::vector<const HelicopterState*> GetTeamHelicopters(uint32_t teamId) const;

    // Engine
    void StartEngine(uint32_t heliId);
    void StopEngine(uint32_t heliId);
    bool IsEngineRunning(uint32_t heliId) const;

    // Control input
    void SetControlInput(uint32_t heliId, const HeliControlInput& input);

    // Occupants
    bool EnterHelicopter(uint32_t playerId, uint32_t heliId, uint32_t seatIndex);
    bool ExitHelicopter(uint32_t playerId);

    // Damage
    void ApplyDamage(uint32_t heliId, float damage, const Vector3& hitPoint);
    void ApplyComponentDamage(uint32_t heliId, float damage, const std::string& component);

    // Per-tick update
    void Update(float deltaSeconds);

    // Configuration
    HeliFlightModel GetFlightModel(HelicopterType type) const;

private:
    GameServer* m_server;
    std::map<uint32_t, HelicopterState> m_helicopters;
    std::map<uint32_t, HeliControlInput> m_controlInputs;
    std::map<uint32_t, HeliFlightModel> m_flightModels;
    uint32_t m_nextHeliId = 1;

    void InitializeFlightModels();
    void InitializeWeaponMounts(HelicopterState& heli);
    void UpdatePhysics(HelicopterState& heli, const HeliControlInput& input, float dt);
    void UpdateRotor(HelicopterState& heli, const HeliFlightModel& model, float dt);
    void UpdateAerodynamics(HelicopterState& heli, const HeliFlightModel& model,
                            const HeliControlInput& input, float dt);
    void UpdateGroundCollision(HelicopterState& heli, float dt);
    void UpdateDamageEffects(HelicopterState& heli, float dt);
    void UpdateWeapons(HelicopterState& heli, const HeliControlInput& input, float dt);
    void CheckCrash(HelicopterState& heli);
    float GetTerrainHeight(const Vector3& position) const;
    Vector3 CalculateLift(const HelicopterState& heli, const HeliFlightModel& model) const;
    Vector3 CalculateDrag(const HelicopterState& heli, const HeliFlightModel& model) const;
    Vector3 CalculateGravity(const HeliFlightModel& model) const;
};
