// src/Game/Vehicle.h – Header for Vehicle

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "Math/Vector3.h"

enum class VehicleType {
    Infantry,       // M113 APC, etc.
    Armor,         // M48 Patton, PT-76
    Air,           // UH-1 Huey, OH-6 Loach
    Artillery,     // M101 Howitzer
    Transport      // Supply trucks
};

enum class VehicleState {
    Idle,
    Moving,
    Engaging,
    Destroyed,
    Respawning
};

struct VehicleOccupant {
    uint32_t playerId;
    uint32_t seatIndex;     // 0=driver, 1=gunner, 2+=passengers
    std::string role;       // "driver", "gunner", "passenger"
};

class Vehicle {
public:
    Vehicle(uint32_t id, VehicleType type, const std::string& name);
    ~Vehicle();

    // Lifecycle
    void Initialize(const Vector3& spawnPos);
    void Update(float deltaTime);
    void Destroy();
    void Respawn();

    // Movement and physics
    void SetPosition(const Vector3& pos);
    Vector3 GetPosition() const;
    void SetVelocity(const Vector3& vel);
    Vector3 GetVelocity() const;
    void SetRotation(const Vector3& rot);
    Vector3 GetRotation() const;

    // Health and damage
    void SetHealth(float health);
    float GetHealth() const;
    float GetMaxHealth() const;
    bool IsDestroyed() const;
    void TakeDamage(float damage, uint32_t attackerId = 0);

    // Occupancy
    bool CanEnter(uint32_t playerId, uint32_t seatIndex) const;
    bool EnterVehicle(uint32_t playerId, uint32_t seatIndex);
    bool ExitVehicle(uint32_t playerId);
    void EjectAllOccupants();
    const std::vector<VehicleOccupant>& GetOccupants() const;
    uint32_t GetDriver() const;
    uint32_t GetGunner() const;
    bool HasOccupants() const;

    // State management
    void SetState(VehicleState state);
    VehicleState GetState() const;
    uint32_t GetTeam() const;
    void SetTeam(uint32_t teamId);

    // Accessors
    uint32_t GetId() const;
    VehicleType GetType() const;
    const std::string& GetName() const;
    uint32_t GetMaxOccupants() const;

    // Timing
    bool CanRespawn() const;
    std::chrono::seconds GetRespawnTime() const;

private:
    uint32_t m_id;
    VehicleType m_type;
    std::string m_name;
    uint32_t m_teamId;

    // Transform
    Vector3 m_position;
    Vector3 m_velocity;
    Vector3 m_rotation;

    // Health
    float m_health;
    float m_maxHealth;

    // State
    VehicleState m_state;
    std::vector<VehicleOccupant> m_occupants;
    uint32_t m_maxOccupants;

    // Timing
    std::chrono::steady_clock::time_point m_destroyedTime;
    std::chrono::seconds m_respawnDelay;

    // Internal helpers
    void InitializeVehicleProperties();
    VehicleOccupant* FindOccupant(uint32_t playerId);
    bool IsSeatOccupied(uint32_t seatIndex) const;
};