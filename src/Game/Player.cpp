// src/Game/Player.cpp – Implementation for Player

#include "Game/Player.h"
#include "Utils/Logger.h"
#include <algorithm>

Player::Player(uint32_t id, std::shared_ptr<ClientConnection> connection)
    : m_id(id)
    , m_connection(connection)
    , m_state(PlayerState::Dead)
    , m_position{0,0,0}
    , m_orientation{0,0,1}
    , m_health(0)
    , m_teamId(0)
{
    Logger::Info("Player %u created", m_id);
}

Player::~Player() {
    Logger::Info("Player %u destroyed", m_id);
}

void Player::Initialize(const std::string& name, uint32_t teamId) {
    m_connection->SetPlayerName(name);
    SetTeam(teamId);
    Reset();
    Logger::Info("Player %u initialized as '%s' on team %u", m_id, name.c_str(), teamId);
}

void Player::Reset() {
    m_state = PlayerState::Dead;
    m_health = 100;
    m_inventory.clear();
    m_deathTime = std::chrono::steady_clock::now();
}

void Player::SetState(PlayerState state) {
    m_state = state;
}

PlayerState Player::GetState() const {
    return m_state;
}

void Player::SetPosition(const Vector3& pos) {
    m_position = pos;
    m_connection->SendPositionUpdate(pos);
}

Vector3 Player::GetPosition() const {
    return m_position;
}

void Player::SetOrientation(const Vector3& dir) {
    m_orientation = dir;
    m_connection->SendOrientationUpdate(dir);
}

Vector3 Player::GetOrientation() const {
    return m_orientation;
}

void Player::SetHealth(int hp) {
    m_health = hp;
    m_connection->SendHealthUpdate(hp);
    if (m_health <= 0) {
        m_state = PlayerState::Dead;
        MarkDeath();
    }
}

int Player::GetHealth() const {
    return m_health;
}

bool Player::IsAlive() const {
    return m_state == PlayerState::Alive;
}

void Player::SetTeam(uint32_t teamId) {
    m_teamId = teamId;
    m_connection->SendTeamUpdate(teamId);
}

uint32_t Player::GetTeam() const {
    return m_teamId;
}

void Player::AddItem(const std::string& name, int qty) {
    auto it = std::find_if(m_inventory.begin(), m_inventory.end(),
        [&](const InventoryItem& item){ return item.name == name; });
    if (it != m_inventory.end()) {
        it->quantity += qty;
    } else {
        m_inventory.push_back({name, qty});
    }
    m_connection->SendInventoryUpdate(m_inventory);
}

bool Player::RemoveItem(const std::string& name, int qty) {
    auto it = std::find_if(m_inventory.begin(), m_inventory.end(),
        [&](const InventoryItem& item){ return item.name == name; });
    if (it == m_inventory.end() || it->quantity < qty) {
        return false;
    }
    it->quantity -= qty;
    if (it->quantity == 0) {
        m_inventory.erase(it);
    }
    m_connection->SendInventoryUpdate(m_inventory);
    return true;
}

const std::vector<InventoryItem>& Player::GetInventory() const {
    return m_inventory;
}

void Player::ClearInventory() {
    m_inventory.clear();
    m_connection->SendInventoryUpdate(m_inventory);
}

void Player::MarkDeath() {
    m_deathTime = std::chrono::steady_clock::now();
}

bool Player::CanRespawn(int respawnDelaySec) const {
    auto elapsed = std::chrono::steady_clock::now() - m_deathTime;
    return elapsed >= std::chrono::seconds(respawnDelaySec);
}

std::shared_ptr<ClientConnection> Player::GetConnection() const {
    return m_connection;
}

void Player::Update(float deltaSeconds) {
    // Example: health regen if alive
    if (m_state == PlayerState::Alive && m_health < 100) {
        m_health = std::min(100, m_health + static_cast<int>(deltaSeconds * 1)); // +1 HP/sec
        m_connection->SendHealthUpdate(m_health);
    }
}