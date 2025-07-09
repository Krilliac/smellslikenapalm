// src/Game/Vehicle.cpp – Implementation for Vehicle

#include "Game/Vehicle.h"
#include "Utils/Logger.h"
#include <algorithm>

Vehicle::Vehicle(uint32_t id, VehicleType type, const std::string& name)
    : m_id(id)
    , m_type(type)
    , m_name(name)
    , m_teamId(0)
    , m_position{0, 0, 0}
    , m_velocity{0, 0, 0}
    , m_rotation{0, 0, 0}
    , m_health(100.0f)
    , m_maxHealth(100.0f)
    , m_state(VehicleState::Idle)
    , m_maxOccupants(1)
    , m_respawnDelay(std::chrono::seconds(120))
{
    Logger::Info("Vehicle %u created: %s (type=%d)", m_id, m_name.c_str(), static_cast<int>(m_type));
    InitializeVehicleProperties();
}

Vehicle::~Vehicle()
{
    Logger::Debug("Vehicle %u destroyed", m_id);
}

void Vehicle::Initialize(const Vector3& spawnPos)
{
    m_position = spawnPos;
    m_health = m_maxHealth;
    m_state = VehicleState::Idle;
    m_occupants.clear();
    
    Logger::Info("Vehicle %u initialized at position (%.1f, %.1f, %.1f)",
                m_id, spawnPos.x, spawnPos.y, spawnPos.z);
}

void Vehicle::Update(float deltaTime)
{
    switch (m_state) {
        case VehicleState::Moving:
            // Update position based on velocity
            m_position.x += m_velocity.x * deltaTime;
            m_position.y += m_velocity.y * deltaTime;
            m_position.z += m_velocity.z * deltaTime;
            break;
            
        case VehicleState::Destroyed:
            // Check if respawn time has elapsed
            if (CanRespawn()) {
                Respawn();
            }
            break;
            
        default:
            break;
    }
}

void Vehicle::Destroy()
{
    if (m_state == VehicleState::Destroyed) return;
    
    Logger::Info("Vehicle %u destroyed", m_id);
    
    // Eject all occupants
    EjectAllOccupants();
    
    // Set destroyed state
    m_state = VehicleState::Destroyed;
    m_health = 0.0f;
    m_destroyedTime = std::chrono::steady_clock::now();
}

void Vehicle::Respawn()
{
    if (m_state != VehicleState::Destroyed) return;
    
    Logger::Info("Vehicle %u respawning", m_id);
    
    m_state = VehicleState::Respawning;
    m_health = m_maxHealth;
    m_velocity = {0, 0, 0};
    m_occupants.clear();
    
    // Position would be set by VehicleManager
    m_state = VehicleState::Idle;
}

void Vehicle::SetPosition(const Vector3& pos)
{
    m_position = pos;
}

Vector3 Vehicle::GetPosition() const
{
    return m_position;
}

void Vehicle::SetVelocity(const Vector3& vel)
{
    m_velocity = vel;
    
    // Update state based on movement
    if (vel.x != 0.0f || vel.y != 0.0f || vel.z != 0.0f) {
        if (m_state == VehicleState::Idle) {
            m_state = VehicleState::Moving;
        }
    } else {
        if (m_state == VehicleState::Moving) {
            m_state = VehicleState::Idle;
        }
    }
}

Vector3 Vehicle::GetVelocity() const
{
    return m_velocity;
}

void Vehicle::SetRotation(const Vector3& rot)
{
    m_rotation = rot;
}

Vector3 Vehicle::GetRotation() const
{
    return m_rotation;
}

void Vehicle::SetHealth(float health)
{
    m_health = std::clamp(health, 0.0f, m_maxHealth);
    
    if (m_health <= 0.0f && m_state != VehicleState::Destroyed) {
        Destroy();
    }
}

float Vehicle::GetHealth() const
{
    return m_health;
}

float Vehicle::GetMaxHealth() const
{
    return m_maxHealth;
}

bool Vehicle::IsDestroyed() const
{
    return m_state == VehicleState::Destroyed;
}

void Vehicle::TakeDamage(float damage, uint32_t attackerId)
{
    if (IsDestroyed()) return;
    
    float newHealth = m_health - damage;
    SetHealth(newHealth);
    
    Logger::Debug("Vehicle %u took %.1f damage from %u (health: %.1f/%.1f)",
                 m_id, damage, attackerId, m_health, m_maxHealth);
}

bool Vehicle::CanEnter(uint32_t playerId, uint32_t seatIndex) const
{
    if (IsDestroyed()) return false;
    if (seatIndex >= m_maxOccupants) return false;
    if (IsSeatOccupied(seatIndex)) return false;
    if (FindOccupant(playerId) != nullptr) return false; // Player already in vehicle
    
    return true;
}

bool Vehicle::EnterVehicle(uint32_t playerId, uint32_t seatIndex)
{
    if (!CanEnter(playerId, seatIndex)) {
        return false;
    }
    
    VehicleOccupant occupant;
    occupant.playerId = playerId;
    occupant.seatIndex = seatIndex;
    
    // Assign role based on seat
    switch (seatIndex) {
        case 0:
            occupant.role = "driver";
            break;
        case 1:
            occupant.role = "gunner";
            break;
        default:
            occupant.role = "passenger";
            break;
    }
    
    m_occupants.push_back(occupant);
    
    Logger::Info("Player %u entered vehicle %u as %s (seat %u)",
                playerId, m_id, occupant.role.c_str(), seatIndex);
    
    return true;
}

bool Vehicle::ExitVehicle(uint32_t playerId)
{
    auto it = std::find_if(m_occupants.begin(), m_occupants.end(),
                          [playerId](const VehicleOccupant& occ) {
                              return occ.playerId == playerId;
                          });
    
    if (it != m_occupants.end()) {
        Logger::Info("Player %u exited vehicle %u", playerId, m_id);
        m_occupants.erase(it);
        return true;
    }
    
    return false;
}

void Vehicle::EjectAllOccupants()
{
    for (const auto& occupant : m_occupants) {
        Logger::Info("Player %u ejected from vehicle %u", occupant.playerId, m_id);
    }
    m_occupants.clear();
}

const std::vector<VehicleOccupant>& Vehicle::GetOccupants() const
{
    return m_occupants;
}

uint32_t Vehicle::GetDriver() const
{
    for (const auto& occupant : m_occupants) {
        if (occupant.seatIndex == 0) {
            return occupant.playerId;
        }
    }
    return 0; // No driver
}

uint32_t Vehicle::GetGunner() const
{
    for (const auto& occupant : m_occupants) {
        if (occupant.seatIndex == 1) {
            return occupant.playerId;
        }
    }
    return 0; // No gunner
}

bool Vehicle::HasOccupants() const
{
    return !m_occupants.empty();
}

void Vehicle::SetState(VehicleState state)
{
    m_state = state;
}

VehicleState Vehicle::GetState() const
{
    return m_state;
}

uint32_t Vehicle::GetTeam() const
{
    return m_teamId;
}

void Vehicle::SetTeam(uint32_t teamId)
{
    m_teamId = teamId;
}

uint32_t Vehicle::GetId() const
{
    return m_id;
}

VehicleType Vehicle::GetType() const
{
    return m_type;
}

const std::string& Vehicle::GetName() const
{
    return m_name;
}

uint32_t Vehicle::GetMaxOccupants() const
{
    return m_maxOccupants;
}

bool Vehicle::CanRespawn() const
{
    if (m_state != VehicleState::Destroyed) return false;
    
    auto elapsed = std::chrono::steady_clock::now() - m_destroyedTime;
    return elapsed >= m_respawnDelay;
}

std::chrono::seconds Vehicle::GetRespawnTime() const
{
    return m_respawnDelay;
}

void Vehicle::InitializeVehicleProperties()
{
    // Set vehicle-specific properties based on type
    switch (m_type) {
        case VehicleType::Infantry:
            m_maxHealth = 150.0f;
            m_maxOccupants = 4;  // Driver + 3 passengers
            m_respawnDelay = std::chrono::seconds(90);
            break;
            
        case VehicleType::Armor:
            m_maxHealth = 300.0f;
            m_maxOccupants = 3;  // Driver + gunner + commander
            m_respawnDelay = std::chrono::seconds(180);
            break;
            
        case VehicleType::Air:
            m_maxHealth = 100.0f;
            m_maxOccupants = 6;  // Pilot + copilot + 4 passengers
            m_respawnDelay = std::chrono::seconds(240);
            break;
            
        case VehicleType::Artillery:
            m_maxHealth = 80.0f;
            m_maxOccupants = 2;  // Operator + loader
            m_respawnDelay = std::chrono::seconds(120);
            break;
            
        case VehicleType::Transport:
            m_maxHealth = 100.0f;
            m_maxOccupants = 8;  // Driver + 7 passengers
            m_respawnDelay = std::chrono::seconds(60);
            break;
    }
    
    m_health = m_maxHealth;
    
    Logger::Debug("Vehicle %u properties: health=%.1f, maxOccupants=%u, respawnDelay=%lds",
                 m_id, m_maxHealth, m_maxOccupants, m_respawnDelay.count());
}

VehicleOccupant* Vehicle::FindOccupant(uint32_t playerId)
{
    auto it = std::find_if(m_occupants.begin(), m_occupants.end(),
                          [playerId](const VehicleOccupant& occ) {
                              return occ.playerId == playerId;
                          });
    
    return it != m_occupants.end() ? &(*it) : nullptr;
}

bool Vehicle::IsSeatOccupied(uint32_t seatIndex) const
{
    return std::any_of(m_occupants.begin(), m_occupants.end(),
                      [seatIndex](const VehicleOccupant& occ) {
                          return occ.seatIndex == seatIndex;
                      });
}