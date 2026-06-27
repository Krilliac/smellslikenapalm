// src/Game/Player.h – Header for Player

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include "Math/Vector3.h"
#include "Network/ClientConnection.h"

enum class PlayerState {
    Alive,
    Dead,
    Spectating
};

struct InventoryItem {
    std::string name;
    int         quantity;
};

class Player {
public:
    Player(uint32_t id, std::shared_ptr<ClientConnection> connection);
    ~Player();

    void Initialize(const std::string& name, uint32_t teamId);
    void Reset();

    // State
    void SetState(PlayerState state);
    PlayerState GetState() const;

    // Position & movement
    void SetPosition(const Vector3& pos);
    Vector3 GetPosition() const;
    void SetOrientation(const Vector3& dir);
    Vector3 GetOrientation() const;

    // Health
    void SetHealth(int hp);
    int  GetHealth() const;
    bool IsAlive() const;

    // Team
    void SetTeam(uint32_t teamId);
    uint32_t GetTeam() const;

    // God mode (admin/dev testing): when set, DamageSystem ignores all incoming
    // damage to this player. Persists across respawns until explicitly cleared.
    void SetGodMode(bool enabled) { m_godMode = enabled; }
    bool IsGodMode() const { return m_godMode; }

    // Inventory
    void AddItem(const std::string& name, int qty);
    bool RemoveItem(const std::string& name, int qty);
    const std::vector<InventoryItem>& GetInventory() const;
    void ClearInventory();

    // Timing
    void MarkDeath();
    bool CanRespawn(int respawnDelaySec) const;

    // Networking
    std::shared_ptr<ClientConnection> GetConnection() const;

    // Updates
    void Update(float deltaSeconds);

private:
    uint32_t m_id;
    std::shared_ptr<ClientConnection> m_connection;

    PlayerState m_state;
    Vector3     m_position;
    Vector3     m_orientation;
    int         m_health;
    uint32_t    m_teamId;
    bool        m_godMode = false;

    std::vector<InventoryItem> m_inventory;

    std::chrono::steady_clock::time_point m_deathTime;
};