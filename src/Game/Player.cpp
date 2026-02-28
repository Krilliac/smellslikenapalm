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
    Logger::Trace("[Player::Player] Entry: id=%u, connection=%p", id, connection.get());
    Logger::Info("Player %u created", m_id);
    Logger::Debug("[Player::Player] Initial state: Dead, health=0, teamId=0, pos=(0,0,0)");
    Logger::Trace("[Player::Player] Exit");
}

Player::~Player() {
    Logger::Trace("[Player::~Player] Entry: id=%u", m_id);
    Logger::Info("Player %u destroyed", m_id);
    Logger::Trace("[Player::~Player] Exit");
}

void Player::Initialize(const std::string& name, uint32_t teamId) {
    Logger::Trace("[Player::Initialize] Entry: id=%u, name='%s', teamId=%u", m_id, name.c_str(), teamId);
    m_connection->SetPlayerName(name);
    SetTeam(teamId);
    Reset();
    Logger::Info("Player %u initialized as '%s' on team %u", m_id, name.c_str(), teamId);
    Logger::Trace("[Player::Initialize] Exit");
}

void Player::Reset() {
    Logger::Trace("[Player::Reset] Entry: id=%u", m_id);
    Logger::Debug("[Player::Reset] Player %u resetting: state=%d -> Dead, health -> 100, clearing inventory (%zu items)",
                  m_id, static_cast<int>(m_state), m_inventory.size());
    m_state = PlayerState::Dead;
    m_health = 100;
    m_inventory.clear();
    m_deathTime = std::chrono::steady_clock::now();
    Logger::Trace("[Player::Reset] Exit");
}

void Player::SetState(PlayerState state) {
    Logger::Trace("[Player::SetState] Entry: id=%u, newState=%d, oldState=%d", m_id, static_cast<int>(state), static_cast<int>(m_state));
    Logger::Debug("[Player::SetState] Player %u state transition: %d -> %d", m_id, static_cast<int>(m_state), static_cast<int>(state));
    m_state = state;
    Logger::Trace("[Player::SetState] Exit");
}

PlayerState Player::GetState() const {
    Logger::Trace("[Player::GetState] Entry: id=%u, returning state=%d", m_id, static_cast<int>(m_state));
    return m_state;
}

void Player::SetPosition(const Vector3& pos) {
    Logger::Trace("[Player::SetPosition] Entry: id=%u, pos=(%.2f,%.2f,%.2f)", m_id, pos.x, pos.y, pos.z);
    m_position = pos;
    m_connection->SendPositionUpdate(pos);
    Logger::Trace("[Player::SetPosition] Exit");
}

Vector3 Player::GetPosition() const {
    Logger::Trace("[Player::GetPosition] Entry: id=%u, returning (%.2f,%.2f,%.2f)", m_id, m_position.x, m_position.y, m_position.z);
    return m_position;
}

void Player::SetOrientation(const Vector3& dir) {
    Logger::Trace("[Player::SetOrientation] Entry: id=%u, dir=(%.2f,%.2f,%.2f)", m_id, dir.x, dir.y, dir.z);
    m_orientation = dir;
    m_connection->SendOrientationUpdate(dir);
    Logger::Trace("[Player::SetOrientation] Exit");
}

Vector3 Player::GetOrientation() const {
    Logger::Trace("[Player::GetOrientation] Entry: id=%u, returning (%.2f,%.2f,%.2f)", m_id, m_orientation.x, m_orientation.y, m_orientation.z);
    return m_orientation;
}

void Player::SetHealth(int hp) {
    Logger::Trace("[Player::SetHealth] Entry: id=%u, newHp=%d, oldHp=%d", m_id, hp, m_health);
    m_health = hp;
    m_connection->SendHealthUpdate(hp);
    if (m_health <= 0) {
        Logger::Debug("[Player::SetHealth] Player %u health dropped to %d, transitioning to Dead state", m_id, m_health);
        m_state = PlayerState::Dead;
        MarkDeath();
    } else {
        Logger::Debug("[Player::SetHealth] Player %u health set to %d", m_id, m_health);
    }
    Logger::Trace("[Player::SetHealth] Exit");
}

int Player::GetHealth() const {
    Logger::Trace("[Player::GetHealth] Entry: id=%u, returning health=%d", m_id, m_health);
    return m_health;
}

bool Player::IsAlive() const {
    bool alive = m_state == PlayerState::Alive;
    Logger::Trace("[Player::IsAlive] Entry: id=%u, returning %d (state=%d)", m_id, alive, static_cast<int>(m_state));
    return alive;
}

void Player::SetTeam(uint32_t teamId) {
    Logger::Trace("[Player::SetTeam] Entry: id=%u, newTeamId=%u, oldTeamId=%u", m_id, teamId, m_teamId);
    Logger::Debug("[Player::SetTeam] Player %u team change: %u -> %u", m_id, m_teamId, teamId);
    m_teamId = teamId;
    m_connection->SendTeamUpdate(teamId);
    Logger::Trace("[Player::SetTeam] Exit");
}

uint32_t Player::GetTeam() const {
    Logger::Trace("[Player::GetTeam] Entry: id=%u, returning teamId=%u", m_id, m_teamId);
    return m_teamId;
}

void Player::AddItem(const std::string& name, int qty) {
    Logger::Trace("[Player::AddItem] Entry: id=%u, name='%s', qty=%d", m_id, name.c_str(), qty);
    auto it = std::find_if(m_inventory.begin(), m_inventory.end(),
        [&](const InventoryItem& item){ return item.name == name; });
    if (it != m_inventory.end()) {
        Logger::Debug("[Player::AddItem] Player %u already has '%s' (qty=%d), adding %d more", m_id, name.c_str(), it->quantity, qty);
        it->quantity += qty;
    } else {
        Logger::Debug("[Player::AddItem] Player %u adding new item '%s' qty=%d", m_id, name.c_str(), qty);
        m_inventory.push_back({name, qty});
    }
    m_connection->SendInventoryUpdate(m_inventory);
    Logger::Trace("[Player::AddItem] Exit: inventory size=%zu", m_inventory.size());
}

bool Player::RemoveItem(const std::string& name, int qty) {
    Logger::Trace("[Player::RemoveItem] Entry: id=%u, name='%s', qty=%d", m_id, name.c_str(), qty);
    auto it = std::find_if(m_inventory.begin(), m_inventory.end(),
        [&](const InventoryItem& item){ return item.name == name; });
    if (it == m_inventory.end() || it->quantity < qty) {
        Logger::Debug("[Player::RemoveItem] Player %u cannot remove '%s' x%d: %s",
                      m_id, name.c_str(), qty,
                      it == m_inventory.end() ? "item not found" : "insufficient quantity");
        Logger::Trace("[Player::RemoveItem] Exit: return false");
        return false;
    }
    it->quantity -= qty;
    Logger::Debug("[Player::RemoveItem] Player %u removed '%s' x%d, remaining=%d", m_id, name.c_str(), qty, it->quantity);
    if (it->quantity == 0) {
        Logger::Debug("[Player::RemoveItem] Player %u item '%s' depleted, removing from inventory", m_id, name.c_str());
        m_inventory.erase(it);
    }
    m_connection->SendInventoryUpdate(m_inventory);
    Logger::Trace("[Player::RemoveItem] Exit: return true, inventory size=%zu", m_inventory.size());
    return true;
}

const std::vector<InventoryItem>& Player::GetInventory() const {
    Logger::Trace("[Player::GetInventory] Entry: id=%u, inventory size=%zu", m_id, m_inventory.size());
    return m_inventory;
}

void Player::ClearInventory() {
    Logger::Trace("[Player::ClearInventory] Entry: id=%u, clearing %zu items", m_id, m_inventory.size());
    m_inventory.clear();
    m_connection->SendInventoryUpdate(m_inventory);
    Logger::Debug("[Player::ClearInventory] Player %u inventory cleared", m_id);
    Logger::Trace("[Player::ClearInventory] Exit");
}

void Player::MarkDeath() {
    Logger::Trace("[Player::MarkDeath] Entry: id=%u", m_id);
    m_deathTime = std::chrono::steady_clock::now();
    Logger::Debug("[Player::MarkDeath] Player %u death time recorded", m_id);
    Logger::Trace("[Player::MarkDeath] Exit");
}

bool Player::CanRespawn(int respawnDelaySec) const {
    Logger::Trace("[Player::CanRespawn] Entry: id=%u, respawnDelaySec=%d", m_id, respawnDelaySec);
    auto elapsed = std::chrono::steady_clock::now() - m_deathTime;
    bool canRespawn = elapsed >= std::chrono::seconds(respawnDelaySec);
    Logger::Debug("[Player::CanRespawn] Player %u elapsed since death: %lld ms, required: %d s, canRespawn=%d",
                  m_id, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), respawnDelaySec, canRespawn);
    Logger::Trace("[Player::CanRespawn] Exit: return %d", canRespawn);
    return canRespawn;
}

std::shared_ptr<ClientConnection> Player::GetConnection() const {
    Logger::Trace("[Player::GetConnection] Entry: id=%u", m_id);
    return m_connection;
}

void Player::Update(float deltaSeconds) {
    Logger::Trace("[Player::Update] Entry: id=%u, deltaSeconds=%.4f, state=%d, health=%d", m_id, deltaSeconds, static_cast<int>(m_state), m_health);
    // Example: health regen if alive
    if (m_state == PlayerState::Alive && m_health < 100) {
        int oldHealth = m_health;
        m_health = std::min(100, m_health + static_cast<int>(deltaSeconds * 1)); // +1 HP/sec
        m_connection->SendHealthUpdate(m_health);
        if (m_health != oldHealth) {
            Logger::Debug("[Player::Update] Player %u health regen: %d -> %d", m_id, oldHealth, m_health);
        }
    } else {
        Logger::Trace("[Player::Update] Player %u no regen needed: state=%d, health=%d", m_id, static_cast<int>(m_state), m_health);
    }
    Logger::Trace("[Player::Update] Exit");
}
