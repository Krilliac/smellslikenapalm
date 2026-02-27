// src/Game/CommanderAbilities.cpp
// RS2V commander fire support implementation

#include "Game/CommanderAbilities.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Network/NetworkManager.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cstring>

CommanderAbilities::CommanderAbilities(GameServer* server)
    : m_server(server)
{
}

CommanderAbilities::~CommanderAbilities() {
    Shutdown();
}

void CommanderAbilities::Initialize() {
    m_activeEffects.clear();
    m_teamCooldowns.clear();
    Logger::Info("CommanderAbilities initialized");
}

void CommanderAbilities::Shutdown() {
    m_activeEffects.clear();
    m_teamCooldowns.clear();
}

void CommanderAbilities::InitializeTeamAbilities(uint32_t teamId, Faction faction) {
    std::vector<AbilityCooldown> abilities;

    auto addAbility = [&](AbilityType type, float cooldown, int uses = -1) {
        abilities.push_back({type, cooldown, 0.0f, true, uses});
    };

    switch (faction) {
        case Faction::USArmy:
        case Faction::USMC:
        case Faction::AusArmy:
            addAbility(AbilityType::Artillery,        120.0f);
            addAbility(AbilityType::Napalm,           180.0f);
            addAbility(AbilityType::AntiAirArtillery,  90.0f);
            addAbility(AbilityType::Spooky,           300.0f);
            addAbility(AbilityType::CarpetBomb,       600.0f, 1);  // Once per round
            addAbility(AbilityType::ReconPlane,        60.0f);
            addAbility(AbilityType::ForceRespawn,     120.0f);
            addAbility(AbilityType::Resupply,          90.0f);
            break;

        case Faction::NVA:
        case Faction::NLFSV:
            addAbility(AbilityType::MortarBarrage,     90.0f);
            addAbility(AbilityType::RocketBarrage,    150.0f);
            addAbility(AbilityType::AntiAirMissile,    60.0f);
            addAbility(AbilityType::HoChiMinhTrail,   180.0f);
            addAbility(AbilityType::AmbushTrap,        45.0f);
            addAbility(AbilityType::ReconReport,       60.0f);
            addAbility(AbilityType::ForceRespawn,     120.0f);
            addAbility(AbilityType::Resupply,          90.0f);
            break;

        default:
            break;
    }

    m_teamCooldowns[teamId] = std::move(abilities);
    Logger::Info("Commander abilities initialized for team %u (%zu abilities)",
                 teamId, m_teamCooldowns[teamId].size());
}

bool CommanderAbilities::RequestAbility(uint32_t commanderId, AbilityType type,
                                         const Vector3& target, const Vector3& direction) {
    auto* tm = m_server->GetTeamManager();
    uint32_t teamId = tm->GetPlayerTeam(commanderId);
    if (teamId == 0) {
        Logger::Warn("Commander %u not on any team", commanderId);
        return false;
    }

    // Verify cooldown state
    auto* cd = FindCooldown(teamId, type);
    if (!cd || !cd->isAvailable) {
        Logger::Warn("Ability %s not available for team %u (cooldown: %.1fs)",
                     GetAbilityName(type).c_str(), teamId,
                     cd ? cd->remainingSeconds : -1.0f);
        return false;
    }

    if (cd->usesRemaining == 0) {
        Logger::Warn("Ability %s has no uses remaining", GetAbilityName(type).c_str());
        return false;
    }

    // Create the fire support effect
    ActiveFireSupport effect = CreateEffect(type, commanderId, teamId, target, direction);
    effect.id = m_nextEffectId++;
    m_activeEffects.push_back(effect);

    // Start cooldown
    cd->isAvailable = false;
    cd->remainingSeconds = cd->cooldownSeconds;
    if (cd->usesRemaining > 0) cd->usesRemaining--;

    Logger::Info("Commander %u (team %u) called %s at (%.1f, %.1f, %.1f)",
                 commanderId, teamId, GetAbilityName(type).c_str(),
                 target.x, target.y, target.z);

    BroadcastFireSupportEvent(effect);
    return true;
}

void CommanderAbilities::Update(float deltaSeconds) {
    // Update cooldowns
    for (auto& [teamId, abilities] : m_teamCooldowns) {
        for (auto& cd : abilities) {
            if (!cd.isAvailable && cd.remainingSeconds > 0.0f) {
                cd.remainingSeconds -= deltaSeconds;
                if (cd.remainingSeconds <= 0.0f) {
                    cd.remainingSeconds = 0.0f;
                    if (cd.usesRemaining != 0) {
                        cd.isAvailable = true;
                    }
                }
            }
        }
    }

    // Update active effects
    for (auto& effect : m_activeEffects) {
        if (!effect.active) continue;

        effect.elapsed += deltaSeconds;
        effect.tickAccumulator += deltaSeconds;

        // Apply damage at tick intervals
        while (effect.tickAccumulator >= effect.tickInterval) {
            effect.tickAccumulator -= effect.tickInterval;
            ApplyFireSupportDamage(effect);

            // Handle multi-wave effects
            if (effect.totalWaves > 1) {
                float waveTime = effect.currentWave * effect.timeBetweenWaves;
                if (effect.elapsed >= waveTime && effect.currentWave < effect.totalWaves) {
                    effect.currentWave++;
                }
            }
        }

        // Check if effect has expired
        if (effect.elapsed >= effect.duration) {
            effect.active = false;
            Logger::Info("Fire support effect %u (%s) expired",
                         effect.id, GetAbilityName(effect.type).c_str());
        }
    }

    // Remove expired effects
    m_activeEffects.erase(
        std::remove_if(m_activeEffects.begin(), m_activeEffects.end(),
                       [](const ActiveFireSupport& e) { return !e.active; }),
        m_activeEffects.end());
}

void CommanderAbilities::ApplyFireSupportDamage(const ActiveFireSupport& effect) {
    auto* pm = m_server->GetPlayerManager();
    auto* tm = m_server->GetTeamManager();
    if (!pm || !tm) return;

    for (auto& player : pm->GetAlivePlayers()) {
        uint32_t pid = player->GetConnection()->GetClientId();
        uint32_t playerTeam = tm->GetPlayerTeam(pid);

        // Don't damage friendly team (unless friendly fire enabled)
        if (playerTeam == effect.teamId) continue;

        Vector3 playerPos = player->GetPosition();
        float dist = playerPos.Distance(effect.targetPosition);

        if (dist <= effect.radius) {
            // Damage falls off with distance from center
            float falloff = 1.0f - (dist / effect.radius);
            falloff = falloff * falloff;  // quadratic falloff
            float damage = effect.damagePerSecond * effect.tickInterval * falloff;

            int newHealth = player->GetHealth() - static_cast<int>(damage);
            player->SetHealth(newHealth);

            if (newHealth <= 0) {
                pm->OnPlayerDeath(pid);
                Logger::Info("Player %u killed by %s fire support",
                             pid, GetAbilityName(effect.type).c_str());
            }
        }
    }

    if (m_fireSupportCallback) {
        m_fireSupportCallback(effect);
    }
}

void CommanderAbilities::BroadcastFireSupportEvent(const ActiveFireSupport& effect) const {
    std::vector<uint8_t> data;
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&effect.id),
                reinterpret_cast<const uint8_t*>(&effect.id) + sizeof(effect.id));
    uint8_t typeVal = static_cast<uint8_t>(effect.type);
    data.push_back(typeVal);
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&effect.teamId),
                reinterpret_cast<const uint8_t*>(&effect.teamId) + sizeof(effect.teamId));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&effect.targetPosition),
                reinterpret_cast<const uint8_t*>(&effect.targetPosition) + sizeof(Vector3));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&effect.radius),
                reinterpret_cast<const uint8_t*>(&effect.radius) + sizeof(float));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&effect.duration),
                reinterpret_cast<const uint8_t*>(&effect.duration) + sizeof(float));

    m_server->GetNetworkManager()->BroadcastPacket("FIRE_SUPPORT_EVENT", data);
}

ActiveFireSupport CommanderAbilities::CreateEffect(AbilityType type, uint32_t commanderId,
                                                     uint32_t teamId, const Vector3& target,
                                                     const Vector3& direction) {
    ActiveFireSupport effect;
    effect.type = type;
    effect.commanderId = commanderId;
    effect.teamId = teamId;
    effect.targetPosition = target;
    effect.targetDirection = direction;

    switch (type) {
        case AbilityType::Artillery:
            effect.radius = 40.0f;
            effect.duration = 15.0f;
            effect.damagePerSecond = 300.0f;
            effect.totalWaves = 6;
            effect.timeBetweenWaves = 2.5f;
            break;

        case AbilityType::Napalm:
            effect.radius = 25.0f;
            effect.duration = 20.0f;      // Burns for a long time
            effect.damagePerSecond = 500.0f;
            effect.totalWaves = 1;
            break;

        case AbilityType::AntiAirArtillery:
            effect.radius = 100.0f;       // Large area for AA
            effect.duration = 30.0f;
            effect.damagePerSecond = 100.0f;  // Mainly anti-vehicle
            break;

        case AbilityType::Spooky:
            effect.radius = 60.0f;
            effect.duration = 45.0f;      // Long loiter time
            effect.damagePerSecond = 150.0f;
            break;

        case AbilityType::CarpetBomb:
            effect.radius = 80.0f;
            effect.duration = 8.0f;
            effect.damagePerSecond = 1000.0f;  // Devastating
            effect.totalWaves = 3;
            effect.timeBetweenWaves = 2.0f;
            break;

        case AbilityType::MortarBarrage:
            effect.radius = 35.0f;
            effect.duration = 12.0f;
            effect.damagePerSecond = 250.0f;
            effect.totalWaves = 8;
            effect.timeBetweenWaves = 1.5f;
            break;

        case AbilityType::RocketBarrage:
            effect.radius = 50.0f;
            effect.duration = 10.0f;
            effect.damagePerSecond = 350.0f;
            effect.totalWaves = 4;
            effect.timeBetweenWaves = 2.0f;
            break;

        case AbilityType::ForceRespawn:
            effect.radius = 0.0f;
            effect.duration = 1.0f;
            effect.damagePerSecond = 0.0f;
            break;

        default:
            effect.radius = 30.0f;
            effect.duration = 10.0f;
            effect.damagePerSecond = 100.0f;
            break;
    }

    return effect;
}

// --- Query Methods ---

bool CommanderAbilities::IsAbilityAvailable(uint32_t teamId, AbilityType type) const {
    const auto* cd = FindCooldown(teamId, type);
    return cd && cd->isAvailable;
}

float CommanderAbilities::GetCooldownRemaining(uint32_t teamId, AbilityType type) const {
    const auto* cd = FindCooldown(teamId, type);
    return cd ? cd->remainingSeconds : 0.0f;
}

std::vector<AbilityCooldown> CommanderAbilities::GetTeamAbilities(uint32_t teamId) const {
    auto it = m_teamCooldowns.find(teamId);
    return it != m_teamCooldowns.end() ? it->second : std::vector<AbilityCooldown>{};
}

std::vector<const ActiveFireSupport*> CommanderAbilities::GetActiveEffects() const {
    std::vector<const ActiveFireSupport*> result;
    for (const auto& e : m_activeEffects) {
        if (e.active) result.push_back(&e);
    }
    return result;
}

std::string CommanderAbilities::GetAbilityName(AbilityType type) const {
    switch (type) {
        case AbilityType::Artillery:        return "Artillery Barrage";
        case AbilityType::Napalm:           return "Napalm Strike";
        case AbilityType::AntiAirArtillery: return "Anti-Air Artillery";
        case AbilityType::Spooky:           return "AC-47 Spooky";
        case AbilityType::CarpetBomb:       return "B-52 Carpet Bomb";
        case AbilityType::ReconPlane:       return "Recon Plane";
        case AbilityType::MortarBarrage:    return "Mortar Barrage";
        case AbilityType::RocketBarrage:    return "Rocket Barrage";
        case AbilityType::AntiAirMissile:   return "SA-7 Anti-Air";
        case AbilityType::HoChiMinhTrail:   return "Ho Chi Minh Trail";
        case AbilityType::AmbushTrap:       return "Ambush Trap";
        case AbilityType::ForceRespawn:     return "Force Respawn";
        case AbilityType::ReconReport:      return "Recon Report";
        case AbilityType::Resupply:         return "Resupply";
        default:                            return "Unknown";
    }
}

void CommanderAbilities::SetOnFireSupportActive(FireSupportCallback cb) {
    m_fireSupportCallback = std::move(cb);
}

void CommanderAbilities::SetRequiresRadioman(bool value) {
    m_requiresRadioman = value;
}

bool CommanderAbilities::GetRequiresRadioman() const {
    return m_requiresRadioman;
}

AbilityCooldown* CommanderAbilities::FindCooldown(uint32_t teamId, AbilityType type) {
    auto it = m_teamCooldowns.find(teamId);
    if (it == m_teamCooldowns.end()) return nullptr;
    for (auto& cd : it->second) {
        if (cd.type == type) return &cd;
    }
    return nullptr;
}

const AbilityCooldown* CommanderAbilities::FindCooldown(uint32_t teamId, AbilityType type) const {
    auto it = m_teamCooldowns.find(teamId);
    if (it == m_teamCooldowns.end()) return nullptr;
    for (const auto& cd : it->second) {
        if (cd.type == type) return &cd;
    }
    return nullptr;
}
