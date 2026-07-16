// src/Game/ObjectiveSystem.cpp
// RS2V objective/capture zone system implementation

#include "Game/ObjectiveSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/RoundManager.h"
#include "Game/SkirmishMode.h"
#include "Game/SupremacyMode.h"
#include "Game/TeamManager.h"
#include "Game/TeamMapping.h"
#include "Game/TerritoryMode.h"
#include "Network/NetworkManager.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr float kCaptureStrengthEpsilon = 0.00001f;
constexpr size_t kMaxCaptureEventsPerDispatch = 64;

bool IsPlayableTeam(uint32_t teamId) {
    return teamId == TeamMapping::kServerUs ||
           teamId == TeamMapping::kServerNva;
}

float SanitizeNonNegativeFinite(float value) {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

void SanitizeCaptureZoneState(CaptureZone& zone) {
    if (!IsPlayableTeam(zone.controllingTeam)) {
        zone.controllingTeam = 0;
        if (zone.state == CaptureState::Controlled) {
            zone.state = CaptureState::Neutral;
        }
    }

    zone.captureProgress = std::isfinite(zone.captureProgress)
        ? std::clamp(zone.captureProgress, 0.0f, 1.0f)
        : 0.0f;
    zone.captureSpeed = SanitizeNonNegativeFinite(zone.captureSpeed);
    zone.decaySpeed = SanitizeNonNegativeFinite(zone.decaySpeed);
    zone.contestDecaySpeed = SanitizeNonNegativeFinite(zone.contestDecaySpeed);
    if (zone.cappingTeam != TeamMapping::kRetailUs &&
        zone.cappingTeam != TeamMapping::kRetailNva) {
        zone.cappingTeam = 0xFF;
    }

    switch (zone.type) {
        case ObjectiveType::CapturePoint:
        case ObjectiveType::Territory:
        case ObjectiveType::DestroyTarget:
        case ObjectiveType::HoldZone:
            break;
        default:
            zone.type = ObjectiveType::CapturePoint;
            break;
    }
    switch (zone.state) {
        case CaptureState::Neutral:
        case CaptureState::Contested:
        case CaptureState::Capturing:
        case CaptureState::Controlled:
        case CaptureState::Locked:
            break;
        default:
            zone.state = zone.controllingTeam == 0
                ? CaptureState::Neutral
                : CaptureState::Controlled;
            break;
    }

    if (!zone.enabled) {
        zone.isActive = false;
        zone.state = CaptureState::Locked;
        zone.captureProgress = 0.0f;
        zone.cappingTeam = 0xFF;
        zone.attackerIds.clear();
        zone.defenderIds.clear();
        zone.botCaptureWeightByTeam.fill(0.0f);
    }
}

class ScopedFlag final {
public:
    explicit ScopedFlag(bool& flag) : m_flag(flag) { m_flag = true; }
    ~ScopedFlag() { m_flag = false; }

    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;

private:
    bool& m_flag;
};

float SanitizeCaptureStrength(float strength) {
    return SanitizeNonNegativeFinite(strength);
}

bool HasCaptureStrength(float strength) {
    return strength > kCaptureStrengthEpsilon;
}

bool CaptureStrengthsEqual(float left, float right) {
    return std::fabs(left - right) <= kCaptureStrengthEpsilon;
}

float CaptureSpeedMultiplier(float netCaptureStrength) {
    if (!HasCaptureStrength(netCaptureStrength)) return 0.0f;

    // This is the continuous form of the existing 1.0, 1.5, 2.0, ...
    // diminishing-return curve. Integer human counts therefore behave exactly
    // as before, while (for example) a net strength of 1.5 yields 1.25x.
    return 0.5f + 0.5f * std::min(netCaptureStrength, 5.0f);
}

} // namespace

ObjectiveSystem::ObjectiveSystem(GameServer* server)
    : m_server(server)
{
}

ObjectiveSystem::~ObjectiveSystem() {
    Shutdown();
}

void ObjectiveSystem::Initialize() {
    m_objectives.clear();
    m_initialControllingTeams.clear();
    m_territoryOrder.clear();
    m_pendingCaptureEvents.clear();
    m_currentTerritoryIndex = 0;
    m_territoryAdvancingTeam = 0;
    m_nextObjectiveId = 1;
    m_retailReplicationAccumulator = 0.0f;
    m_capturedCallback = {};
    m_botCaptureWeightProvider = {};
    ++m_objectiveGeneration;
    ++m_botCaptureProviderGeneration;
    Logger::Info("ObjectiveSystem initialized");
}

void ObjectiveSystem::Shutdown() {
    m_objectives.clear();
    m_initialControllingTeams.clear();
    m_territoryOrder.clear();
    m_pendingCaptureEvents.clear();
    m_currentTerritoryIndex = 0;
    m_territoryAdvancingTeam = 0;
    m_nextObjectiveId = 1;
    m_retailReplicationAccumulator = 0.0f;
    m_capturedCallback = {};
    m_botCaptureWeightProvider = {};
    ++m_objectiveGeneration;
    ++m_botCaptureProviderGeneration;
}

uint32_t ObjectiveSystem::AllocateObjectiveId() {
    if (m_objectives.size() >=
        static_cast<size_t>(std::numeric_limits<uint32_t>::max() - 1u)) {
        return 0;
    }

    uint32_t candidate = m_nextObjectiveId == 0 ? 1u : m_nextObjectiveId;
    // With N occupied non-zero ids, inspecting N+1 candidates must find a gap.
    // This keeps malformed sparse/high ids from wrapping into id zero or
    // silently replacing an existing objective.
    for (size_t attempt = 0; attempt <= m_objectives.size(); ++attempt) {
        if (m_objectives.find(candidate) == m_objectives.end()) {
            m_nextObjectiveId = candidate == std::numeric_limits<uint32_t>::max()
                ? 1u
                : candidate + 1u;
            return candidate;
        }
        candidate = candidate == std::numeric_limits<uint32_t>::max()
            ? 1u
            : candidate + 1u;
    }
    return 0;
}

uint32_t ObjectiveSystem::AddObjective(const CaptureZone& zone) {
    CaptureZone z = zone;
    z.attackerIds.clear();
    z.defenderIds.clear();
    z.botCaptureWeightByTeam.fill(0.0f);
    SanitizeCaptureZoneState(z);
    // Preserve a caller-supplied id (e.g. one loaded from map data) so the
    // ObjectiveSystem, MapManager and GameState all agree on objective ids.
    // Fall back to an auto-assigned id when none was provided (id == 0).
    if (z.id == 0 || m_objectives.find(z.id) != m_objectives.end()) {
        if (z.id != 0) {
            Logger::Warn("Objective id %u is duplicated; assigning a unique runtime id",
                         z.id);
        }
        z.id = AllocateObjectiveId();
        if (z.id == 0) {
            Logger::Error("Objective registry exhausted all non-zero ids");
            return 0;
        }
    } else if (z.id >= m_nextObjectiveId) {
        m_nextObjectiveId = z.id == std::numeric_limits<uint32_t>::max()
            ? 1u
            : z.id + 1u;
    }
    m_objectives[z.id] = z;
    ++m_objectiveGeneration;
    m_initialControllingTeams[z.id] =
        IsPlayableTeam(z.controllingTeam)
            ? z.controllingTeam
            : 0u;
    Logger::Info("Objective added: '%s' (id=%u) at (%.1f, %.1f, %.1f) radius=%.1f",
                 z.name.c_str(), z.id, z.position.x, z.position.y, z.position.z, z.captureRadius);
    return z.id;
}

void ObjectiveSystem::Clear() {
    m_objectives.clear();
    m_initialControllingTeams.clear();
    m_territoryOrder.clear();
    m_pendingCaptureEvents.clear();
    m_currentTerritoryIndex = 0;
    m_territoryAdvancingTeam = 0;
    m_nextObjectiveId = 1;
    m_retailReplicationAccumulator = 0.0f;
    ++m_objectiveGeneration;
    Logger::Debug("ObjectiveSystem cleared");
}

void ObjectiveSystem::RemoveObjective(uint32_t objectiveId) {
    if (objectiveId == 0) return;
    m_objectives.erase(objectiveId);
    m_initialControllingTeams.erase(objectiveId);
    m_pendingCaptureEvents.erase(
        std::remove_if(m_pendingCaptureEvents.begin(),
                       m_pendingCaptureEvents.end(),
                       [objectiveId](const PendingCaptureEvent& event) {
                           return event.objectiveId == objectiveId;
                       }),
        m_pendingCaptureEvents.end());

    const auto orderIt = std::find(m_territoryOrder.begin(),
                                   m_territoryOrder.end(), objectiveId);
    if (orderIt != m_territoryOrder.end()) {
        const int removedIndex = static_cast<int>(
            std::distance(m_territoryOrder.begin(), orderIt));
        m_territoryOrder.erase(orderIt);
        if (removedIndex < m_currentTerritoryIndex) --m_currentTerritoryIndex;
        if (m_currentTerritoryIndex >= static_cast<int>(m_territoryOrder.size())) {
            m_currentTerritoryIndex = static_cast<int>(m_territoryOrder.size());
        }
    }
    ++m_objectiveGeneration;
}

CaptureZone* ObjectiveSystem::GetObjective(uint32_t id) {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() ? &it->second : nullptr;
}

const CaptureZone* ObjectiveSystem::GetObjective(uint32_t id) const {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() ? &it->second : nullptr;
}

std::vector<const CaptureZone*> ObjectiveSystem::GetAllObjectives() const {
    std::vector<const CaptureZone*> result;
    for (const auto& [id, zone] : m_objectives) {
        result.push_back(&zone);
    }
    return result;
}

std::vector<const CaptureZone*> ObjectiveSystem::GetActiveObjectives() const {
    std::vector<const CaptureZone*> result;
    for (const auto& [id, zone] : m_objectives) {
        if (zone.enabled && zone.isActive) result.push_back(&zone);
    }
    return result;
}

void ObjectiveSystem::SetTerritoryOrder(const std::vector<uint32_t>& objectiveIds) {
    std::vector<uint32_t> validatedOrder;
    validatedOrder.reserve(objectiveIds.size());
    for (uint32_t objectiveId : objectiveIds) {
        const CaptureZone* zone = GetObjective(objectiveId);
        if (objectiveId == 0 || !zone || !zone->enabled ||
            zone->type != ObjectiveType::Territory) {
            Logger::Warn("Territory order ignored invalid, disabled, or non-Territory objective id %u",
                         objectiveId);
            continue;
        }
        if (std::find(validatedOrder.begin(), validatedOrder.end(), objectiveId) !=
            validatedOrder.end()) {
            Logger::Warn("Territory order ignored duplicate objective id %u",
                         objectiveId);
            continue;
        }
        validatedOrder.push_back(objectiveId);
    }

    // Progression groups and comparisons operate on CaptureZone::territoryOrder,
    // not the caller's vector position. Normalize malformed/nonmonotonic input
    // here so a phase-1 entry cannot make phase 0 unreachable or report the
    // chain complete early. stable_sort preserves authored order within a
    // simultaneous multi-objective phase.
    const std::vector<uint32_t> suppliedValidatedOrder = validatedOrder;
    std::stable_sort(
        validatedOrder.begin(), validatedOrder.end(),
        [this](uint32_t leftId, uint32_t rightId) {
            const CaptureZone* left = GetObjective(leftId);
            const CaptureZone* right = GetObjective(rightId);
            if (!left || !right) return leftId < rightId;
            return left->territoryOrder < right->territoryOrder;
        });
    if (validatedOrder != suppliedValidatedOrder) {
        Logger::Warn("Territory order was nonmonotonic; normalized by authored phase");
    }

    m_territoryOrder = std::move(validatedOrder);
    m_currentTerritoryIndex = 0;
    m_territoryAdvancingTeam = 0;
    m_pendingCaptureEvents.clear();
    ++m_objectiveGeneration;

    // Lock all objectives except the first PHASE. Multiple objectives may share
    // a territoryOrder (Resort's Villa/Farm phase) and must be active together.
    for (auto& [id, zone] : m_objectives) {
        zone.isActive = false;
        zone.state = CaptureState::Locked;
        zone.attackerIds.clear();
        zone.defenderIds.clear();
        zone.botCaptureWeightByTeam.fill(0.0f);
    }

    if (!m_territoryOrder.empty()) {
        auto* first = GetObjective(m_territoryOrder[0]);
        if (!first) return;
        const int firstPhase = first->territoryOrder;
        for (uint32_t id : m_territoryOrder) {
            auto* zone = GetObjective(id);
            if (zone && zone->enabled && zone->territoryOrder == firstPhase) {
                zone->isActive = true;
                zone->state = CaptureState::Neutral;
                Logger::Info("Territory mode: first-phase objective '%s' activated",
                             zone->name.c_str());
            }
        }
    }
}

void ObjectiveSystem::ResetTerritoryForRound(uint32_t defendingTeam) {
    if (!IsPlayableTeam(defendingTeam)) {
        Logger::Warn("Territory reset rejected invalid defending team %u",
                     defendingTeam);
        return;
    }

    m_currentTerritoryIndex = 0;
    m_territoryAdvancingTeam = defendingTeam == TeamMapping::kServerUs
        ? TeamMapping::kServerNva
        : TeamMapping::kServerUs;
    m_pendingCaptureEvents.clear();
    ++m_objectiveGeneration;

    int firstPhase = 0;
    bool hasFirstPhase = false;
    if (!m_territoryOrder.empty()) {
        if (const CaptureZone* first = GetObjective(m_territoryOrder.front())) {
            firstPhase = first->territoryOrder;
            hasFirstPhase = true;
        }
    }

    for (auto& [id, zone] : m_objectives) {
        const bool inTerritoryOrder =
            std::find(m_territoryOrder.begin(), m_territoryOrder.end(), id) !=
            m_territoryOrder.end();
        const bool inFirstPhase = hasFirstPhase && zone.enabled &&
                                  inTerritoryOrder &&
                                  zone.territoryOrder == firstPhase;
        zone.controllingTeam = defendingTeam;
        zone.captureProgress = 0.0f;
        zone.cappingTeam = 0xFF;
        zone.attackerIds.clear();
        zone.defenderIds.clear();
        zone.botCaptureWeightByTeam.fill(0.0f);
        zone.isActive = inFirstPhase;
        zone.state = inFirstPhase ? CaptureState::Controlled
                                  : CaptureState::Locked;
    }
    m_retailReplicationAccumulator = 0.0f;

    Logger::Info("Territory mode: reset %zu objectives to defender team %u; first phase=%d",
                 m_objectives.size(), defendingTeam,
                 hasFirstPhase ? firstPhase : -1);
    BroadcastObjectiveStates();
}

void ObjectiveSystem::ResetObjectivesToInitialOwners() {
    m_pendingCaptureEvents.clear();
    ++m_objectiveGeneration;
    for (auto& [id, zone] : m_objectives) {
        const auto initial = m_initialControllingTeams.find(id);
        zone.controllingTeam = initial == m_initialControllingTeams.end()
            ? 0u
            : initial->second;
        zone.captureProgress = 0.0f;
        zone.cappingTeam = 0xFF;
        zone.attackerIds.clear();
        zone.defenderIds.clear();
        zone.botCaptureWeightByTeam.fill(0.0f);
        if (zone.type != ObjectiveType::Territory) {
            zone.isActive = zone.enabled;
        } else if (!zone.enabled) {
            zone.isActive = false;
        }
        zone.state = !zone.enabled || !zone.isActive
            ? CaptureState::Locked
            : (zone.controllingTeam == 0
                   ? CaptureState::Neutral
                   : CaptureState::Controlled);
    }
    m_retailReplicationAccumulator = 0.0f;

    Logger::Info("ObjectiveSystem restored %zu objectives to authored ownership",
                 m_objectives.size());
    BroadcastObjectiveStates();
}

void ObjectiveSystem::ActivateNextTerritory(uint32_t capturingTeamDirection) {
    if (!IsPlayableTeam(capturingTeamDirection)) {
        Logger::Warn("Territory advance rejected invalid capturing team %u",
                     capturingTeamDirection);
        return;
    }
    if (m_territoryAdvancingTeam != 0 &&
        capturingTeamDirection != m_territoryAdvancingTeam) {
        Logger::Info("Territory phase remains active after defender team %u recapture",
                     capturingTeamDirection);
        return;
    }
    if (m_currentTerritoryIndex >= (int)m_territoryOrder.size()) return;
    auto* current = GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
    if (!current) return;
    ++m_objectiveGeneration;
    const int currentPhase = current->territoryOrder;

    // A grouped phase advances only after every objective in that phase belongs
    // to the advancing team. Captured members lock while their siblings remain live.
    bool phaseComplete = true;
    for (uint32_t id : m_territoryOrder) {
        auto* zone = GetObjective(id);
        if (!zone || !zone->enabled || zone->territoryOrder != currentPhase) continue;
        if (zone->controllingTeam == capturingTeamDirection) {
            zone->isActive = false;
            zone->attackerIds.clear();
            zone->defenderIds.clear();
            zone->botCaptureWeightByTeam.fill(0.0f);
        } else {
            phaseComplete = false;
        }
    }
    if (!phaseComplete) {
        Logger::Info("Territory mode: phase %d still has uncaptured objectives", currentPhase);
        return;
    }

    int nextIndex = m_currentTerritoryIndex;
    while (nextIndex < (int)m_territoryOrder.size()) {
        auto* zone = GetObjective(m_territoryOrder[nextIndex]);
        if (zone && zone->territoryOrder > currentPhase) break;
        ++nextIndex;
    }
    m_currentTerritoryIndex = nextIndex;
    if (m_currentTerritoryIndex >= (int)m_territoryOrder.size()) {
        Logger::Info("Territory mode: all objectives captured by advancing team");
        return;
    }

    auto* firstNext = GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
    if (!firstNext) return;
    const int nextPhase = firstNext->territoryOrder;
    for (uint32_t id : m_territoryOrder) {
        auto* zone = GetObjective(id);
        if (zone && zone->enabled && zone->territoryOrder == nextPhase) {
            zone->isActive = true;
            zone->state = zone->controllingTeam == 0
                ? CaptureState::Neutral
                : CaptureState::Controlled;
            zone->captureProgress = 0.0f;
            zone->cappingTeam = 0xFF;
            zone->attackerIds.clear();
            zone->defenderIds.clear();
            zone->botCaptureWeightByTeam.fill(0.0f);
            Logger::Info("Territory mode: next-phase objective '%s' activated (phase %d)",
                         zone->name.c_str(), nextPhase);
        }
    }
}

const CaptureZone* ObjectiveSystem::GetCurrentTerritoryObjective() const {
    if (m_currentTerritoryIndex < (int)m_territoryOrder.size()) {
        const CaptureZone* first = GetObjective(m_territoryOrder[m_currentTerritoryIndex]);
        if (!first) return nullptr;
        const int phase = first->territoryOrder;
        for (size_t i = static_cast<size_t>(m_currentTerritoryIndex);
             i < m_territoryOrder.size(); ++i) {
            const CaptureZone* zone = GetObjective(m_territoryOrder[i]);
            if (!zone || zone->territoryOrder != phase) break;
            if (zone->enabled && zone->isActive) return zone;
        }
        return first && first->enabled ? first : nullptr;
    }
    return nullptr;
}

void ObjectiveSystem::Update(float deltaSeconds) {
    Update(deltaSeconds, deltaSeconds);
}

void ObjectiveSystem::Update(float deltaSeconds, float captureDeltaSeconds) {
    if (m_updateInProgress) {
        Logger::Warn("Objective update ignored a reentrant request");
        return;
    }

    {
        ScopedFlag updateGuard(m_updateInProgress);

        // CaptureZone is intentionally queryable for replication and mode
        // integration. Re-establish authority invariants before consuming any
        // externally-mutated state.
        for (auto& [id, zone] : m_objectives) {
            (void)id;
            SanitizeCaptureZoneState(zone);
        }

        if (CanProcessCaptures()) {
            RefreshPlayerZones();

            for (auto& [id, zone] : m_objectives) {
                (void)id;
                if (!zone.enabled || !zone.isActive) continue;
                ProcessCapture(zone, captureDeltaSeconds);
            }
        } else {
            // Do not leave stale cappers visible on the retail HUD while a native
            // mode is in warmup, preparation, or post-round.
            for (auto& [id, zone] : m_objectives) {
                (void)id;
                zone.attackerIds.clear();
                zone.defenderIds.clear();
                zone.botCaptureWeightByTeam.fill(0.0f);
            }
        }

        // Quantized actor deltas are cheap and ConnectionManager suppresses unchanged
        // bytes. Ten Hz is enough for the HUD capture bar without a per-tick packet/log
        // flood; capture completion still sends immediately below.
        m_retailReplicationAccumulator += std::isfinite(deltaSeconds)
            ? std::max(0.0f, deltaSeconds)
            : 0.0f;
        if (m_retailReplicationAccumulator >= 0.1f) {
            m_retailReplicationAccumulator = 0.0f;
            if (m_server) {
                if (auto* network = m_server->GetNetworkManager()) {
                    network->BroadcastRetailObjectiveState();
                }
            }
        }
    }

    // External callbacks run only after map iteration has ended, so a mode may
    // clear/reset objectives without invalidating the active iterator/reference.
    DispatchPendingCaptureEvents();
}

bool ObjectiveSystem::CanProcessCaptures() const {
    if (!m_server) return true;

    // The opt-in central round manager supersedes native mode clocks.
    if (const auto* rounds = m_server->GetRoundManager()) {
        return rounds->GetCurrentPhase() == RoundPhase::Active;
    }
    if (const auto* territory = m_server->GetTerritoryMode()) {
        return territory->CanCaptureObjectives();
    }
    if (const auto* supremacy = m_server->GetSupremacyMode()) {
        return supremacy->CanCaptureObjectives();
    }
    if (const auto* skirmish = m_server->GetSkirmishMode()) {
        return skirmish->CanCaptureObjectives();
    }

    // Generic modes retain their existing capture behavior.
    return true;
}

void ObjectiveSystem::RefreshPlayerZones() {
    if (m_zoneRefreshInProgress) {
        Logger::Warn("Objective zone refresh ignored a reentrant request");
        return;
    }
    ScopedFlag refreshGuard(m_zoneRefreshInProgress);

    // Clear existing zone occupants
    for (auto& [id, zone] : m_objectives) {
        (void)id;
        zone.attackerIds.clear();
        zone.defenderIds.clear();
        zone.botCaptureWeightByTeam.fill(0.0f);
    }

    // BotManager is intentionally hidden behind this provider boundary. Query
    // only currently playable objectives so inactive Territory phases cannot
    // retain or display stale headless occupancy.
    const ObjectiveBotCaptureWeightProvider provider = m_botCaptureWeightProvider;
    const uint64_t providerGeneration = m_botCaptureProviderGeneration;
    const uint64_t objectiveGeneration = m_objectiveGeneration;
    if (provider) {
        std::vector<uint32_t> activeObjectiveIds;
        activeObjectiveIds.reserve(m_objectives.size());
        for (const auto& [id, zone] : m_objectives) {
            if (zone.enabled && zone.isActive) activeObjectiveIds.push_back(id);
        }

        for (uint32_t objectiveId : activeObjectiveIds) {
            std::array<float, 3> weights{{0.0f, 0.0f, 0.0f}};
            for (uint32_t teamId : {TeamMapping::kServerUs,
                                    TeamMapping::kServerNva}) {
                try {
                    weights[teamId] = SanitizeCaptureStrength(
                        provider(objectiveId, teamId));
                } catch (...) {
                    Logger::Warn("Bot capture provider failed for objective %u team %u; ignoring weight",
                                 objectiveId, teamId);
                    weights[teamId] = 0.0f;
                }

                // A provider is external code and may replace itself or rebuild
                // the objective registry. Never apply an old callback's result
                // to a new provider/map generation.
                if (providerGeneration != m_botCaptureProviderGeneration ||
                    objectiveGeneration != m_objectiveGeneration) {
                    return;
                }
            }

            auto current = m_objectives.find(objectiveId);
            if (current == m_objectives.end() || !current->second.enabled ||
                !current->second.isActive) {
                continue;
            }
            current->second.botCaptureWeightByTeam = weights;
        }
    }

    auto* pm = m_server ? m_server->GetPlayerManager() : nullptr;
    auto* tm = m_server ? m_server->GetTeamManager() : nullptr;
    if (!pm || !tm) return;

    for (auto& player : pm->GetAlivePlayers()) {
        if (!player) continue;
        const auto connection = player->GetConnection();
        if (!connection) continue;
        const uint32_t pid = connection->GetClientId();
        uint32_t playerTeam = tm->GetPlayerTeam(pid);
        if (playerTeam != TeamMapping::kServerUs &&
            playerTeam != TeamMapping::kServerNva) {
            continue;
        }

        for (auto& [id, zone] : m_objectives) {
            if (!zone.enabled || !zone.isActive) continue;
            if (!IsPlayerInZone(pid, zone)) continue;

            if (zone.controllingTeam == 0 || zone.controllingTeam != playerTeam) {
                zone.attackerIds.push_back(pid);
            } else {
                zone.defenderIds.push_back(pid);
            }
        }
    }
}

uint32_t ObjectiveSystem::ProcessNeutralCapture(CaptureZone& zone,
                                                float team1CaptureWeight,
                                                float team2CaptureWeight,
                                                float deltaSeconds) {
    const float elapsed = std::isfinite(deltaSeconds)
        ? std::max(0.0f, deltaSeconds)
        : 0.0f;
    const float team1Strength = SanitizeCaptureStrength(team1CaptureWeight);
    const float team2Strength = SanitizeCaptureStrength(team2CaptureWeight);
    const float captureRate = SanitizeNonNegativeFinite(zone.captureSpeed);
    const float decayRate = SanitizeNonNegativeFinite(zone.decaySpeed);
    zone.captureProgress = std::isfinite(zone.captureProgress)
        ? std::clamp(zone.captureProgress, 0.0f, 1.0f)
        : 0.0f;
    if (zone.cappingTeam != TeamMapping::kRetailUs &&
        zone.cappingTeam != TeamMapping::kRetailNva) {
        zone.cappingTeam = 0xFF;
    }

    if (!HasCaptureStrength(team1Strength) &&
        !HasCaptureStrength(team2Strength)) {
        zone.state = CaptureState::Neutral;
        if (zone.captureProgress > 0.0f) {
            zone.captureProgress = std::max(
                0.0f, zone.captureProgress - decayRate * elapsed);
        }
        if (zone.captureProgress == 0.0f) zone.cappingTeam = 0xFF;
        return 0;
    }

    if (CaptureStrengthsEqual(team1Strength, team2Strength)) {
        // A true mixed-team tie is a standoff. Preserve the partial bar and the
        // team that owns it; iteration order must never decide a neutral point.
        zone.state = CaptureState::Contested;
        return 0;
    }

    const uint32_t dominantTeam = team1Strength > team2Strength
        ? TeamMapping::kServerUs
        : TeamMapping::kServerNva;
    const float netCaptureStrength = std::fabs(team1Strength - team2Strength);
    const float speedMultiplier = CaptureSpeedMultiplier(netCaptureStrength);
    const float progressDelta =
        captureRate * speedMultiplier * elapsed;
    const uint8_t dominantRetailTeam = TeamMapping::ServerToRetail(dominantTeam);
    zone.state = CaptureState::Capturing;

    if (zone.captureProgress <= 0.0f || zone.cappingTeam > 1) {
        zone.captureProgress = 0.0f;
        zone.cappingTeam = dominantRetailTeam;
    }

    if (zone.cappingTeam != dominantRetailTeam) {
        // The scalar progress bar still belongs to the other team. Drain it to
        // neutral first, then apply any remaining simulation time to the new
        // team. Carrying the overshoot makes one loaded tick equivalent to the
        // same elapsed time split across ordinary ticks.
        if (progressDelta < zone.captureProgress) {
            zone.captureProgress -= progressDelta;
            return 0;
        }
        const float remainingProgress = progressDelta - zone.captureProgress;
        zone.cappingTeam = dominantRetailTeam;
        zone.captureProgress = std::min(1.0f, remainingProgress);
        return zone.captureProgress >= 1.0f ? dominantTeam : 0u;
    }

    zone.captureProgress = std::min(1.0f, zone.captureProgress + progressDelta);
    return zone.captureProgress >= 1.0f ? dominantTeam : 0u;
}

void ObjectiveSystem::ProcessCapture(CaptureZone& zone, float deltaSeconds) {
    SanitizeCaptureZoneState(zone);
    float attackers = static_cast<float>(zone.attackerIds.size());
    float defenders = static_cast<float>(zone.defenderIds.size());
    const float elapsed = std::isfinite(deltaSeconds)
        ? std::max(0.0f, deltaSeconds)
        : 0.0f;
    const float team1BotStrength = SanitizeCaptureStrength(
        zone.botCaptureWeightByTeam[TeamMapping::kServerUs]);
    const float team2BotStrength = SanitizeCaptureStrength(
        zone.botCaptureWeightByTeam[TeamMapping::kServerNva]);
    const float captureRate = SanitizeNonNegativeFinite(zone.captureSpeed);
    const float decayRate = SanitizeNonNegativeFinite(zone.decaySpeed);
    const float contestDecayRate =
        SanitizeNonNegativeFinite(zone.contestDecaySpeed);

    if (zone.controllingTeam == 0) {
        auto* teams = m_server ? m_server->GetTeamManager() : nullptr;
        float team1CaptureWeight = team1BotStrength;
        float team2CaptureWeight = team2BotStrength;
        auto countOccupant = [&](uint32_t playerId) {
            if (!teams) return;
            const uint32_t team = teams->GetPlayerTeam(playerId);
            if (team == TeamMapping::kServerUs) team1CaptureWeight += 1.0f;
            else if (team == TeamMapping::kServerNva) team2CaptureWeight += 1.0f;
        };
        for (uint32_t playerId : zone.attackerIds) countOccupant(playerId);
        for (uint32_t playerId : zone.defenderIds) countOccupant(playerId);

        const uint32_t capturingTeam = ProcessNeutralCapture(
            zone, team1CaptureWeight, team2CaptureWeight, elapsed);
        if (capturingTeam != 0) OnObjectiveCaptured(zone, capturingTeam);
        return;
    }

    uint32_t botAttackingTeam = 0;
    if (zone.controllingTeam == TeamMapping::kServerUs) {
        defenders += team1BotStrength;
        attackers += team2BotStrength;
        if (HasCaptureStrength(team2BotStrength)) {
            botAttackingTeam = TeamMapping::kServerNva;
        }
    } else if (zone.controllingTeam == TeamMapping::kServerNva) {
        defenders += team2BotStrength;
        attackers += team1BotStrength;
        if (HasCaptureStrength(team1BotStrength)) {
            botAttackingTeam = TeamMapping::kServerUs;
        }
    }

    if (!HasCaptureStrength(attackers) && !HasCaptureStrength(defenders)) {
        // Nobody present: slow decay toward the controlling team's stable state.
        if (zone.captureProgress > 0.0f) {
            zone.captureProgress = std::max(
                0.0f, zone.captureProgress - decayRate * elapsed);
        }
        if (zone.captureProgress == 0.0f) {
            zone.cappingTeam = 0xFF;
            zone.state = CaptureState::Controlled;
        }
        return;
    }

    auto* teams = m_server ? m_server->GetTeamManager() : nullptr;
    uint32_t attackingTeam = 0;
    if (!zone.attackerIds.empty() && teams) {
        attackingTeam = teams->GetPlayerTeam(zone.attackerIds.front());
        if (attackingTeam != TeamMapping::kServerUs &&
            attackingTeam != TeamMapping::kServerNva) {
            attackingTeam = 0;
        }
    }
    if (attackingTeam == 0) attackingTeam = botAttackingTeam;
    if (attackingTeam != 0) {
        zone.cappingTeam = TeamMapping::ServerToRetail(attackingTeam);
    }

    if (HasCaptureStrength(attackers) && HasCaptureStrength(defenders)) {
        // Per ROGameInfoTerritories.CaptureTimer, the greater cap value advances;
        // only a true tie stalls. Human counts and bot weights share the same
        // continuous diminishing-return curve.
        if (attackers > defenders + kCaptureStrengthEpsilon) {
            zone.state = CaptureState::Capturing;
            const float multiplier = CaptureSpeedMultiplier(attackers - defenders);
            zone.captureProgress += captureRate * multiplier * elapsed;
            if (zone.captureProgress >= 1.0f && attackingTeam != 0) {
                zone.captureProgress = 1.0f;
                OnObjectiveCaptured(zone, attackingTeam);
            }
        } else if (defenders > attackers + kCaptureStrengthEpsilon) {
            zone.state = CaptureState::Contested;
            const float multiplier = CaptureSpeedMultiplier(defenders - attackers);
            zone.captureProgress = std::max(
                0.0f,
                zone.captureProgress - contestDecayRate * multiplier * elapsed);
            if (zone.captureProgress == 0.0f) zone.cappingTeam = 0xFF;
        } else {
            // Equal capture strength is a standoff. Retain both the partial bar
            // and its capping-team owner; decay here makes a tied defender
            // presence undo progress despite retail's TeamCapValue equality
            // branch explicitly doing neither advance nor regain.
            zone.state = CaptureState::Contested;
        }
        return;
    }

    if (HasCaptureStrength(attackers) && !HasCaptureStrength(defenders)) {
        zone.state = CaptureState::Capturing;
        const float multiplier = CaptureSpeedMultiplier(attackers);
        zone.captureProgress += captureRate * multiplier * elapsed;

        if (zone.captureProgress >= 1.0f) {
            zone.captureProgress = 1.0f;
            if (attackingTeam == 0) {
                Logger::Warn("[ObjectiveSystem::ProcessCapture] No playable attacking team; skipping capture resolution");
                return;
            }
            OnObjectiveCaptured(zone, attackingTeam);
        }
    }

    if (HasCaptureStrength(defenders) && !HasCaptureStrength(attackers)) {
        zone.state = CaptureState::Controlled;
        if (zone.captureProgress > 0.0f) {
            zone.captureProgress = std::max(
                0.0f, zone.captureProgress - decayRate * 2.0f * elapsed);
        }
        if (zone.captureProgress == 0.0f) zone.cappingTeam = 0xFF;
    }
}

void ObjectiveSystem::OnObjectiveCaptured(CaptureZone& zone, uint32_t newTeam) {
    if (!IsPlayableTeam(newTeam) || !zone.enabled || !zone.isActive) {
        Logger::Warn("Objective %u rejected capture by invalid team %u or while inactive",
                     zone.id, newTeam);
        return;
    }

    const uint32_t previousTeam = zone.controllingTeam;
    if (previousTeam == newTeam) {
        zone.captureProgress = 0.0f;
        zone.cappingTeam = 0xFF;
        zone.state = CaptureState::Controlled;
        return;
    }

    const uint32_t objectiveId = zone.id;
    const ObjectiveType objectiveType = zone.type;
    zone.controllingTeam = newTeam;
    zone.state = CaptureState::Controlled;
    zone.captureProgress = 0.0f;
    zone.cappingTeam = 0xFF;
    zone.attackerIds.clear();
    zone.defenderIds.clear();
    zone.botCaptureWeightByTeam.fill(0.0f);

    Logger::Info("Objective '%s' captured by team %u (was team %u)",
                 zone.name.c_str(), newTeam, previousTeam);

    // In Territory mode, activate next objective
    if (objectiveType == ObjectiveType::Territory) {
        ActivateNextTerritory(newTeam);
    }

    if (m_pendingCaptureEvents.size() >= kMaxCaptureEventsPerDispatch) {
        Logger::Warn("Objective capture event queue is full; dropping objective %u event",
                     objectiveId);
        return;
    }
    m_pendingCaptureEvents.push_back(
        PendingCaptureEvent{objectiveId, newTeam, previousTeam});
}

void ObjectiveSystem::DispatchPendingCaptureEvents() {
    if (m_dispatchingCaptureEvents || m_pendingCaptureEvents.empty()) return;
    ScopedFlag dispatchGuard(m_dispatchingCaptureEvents);

    size_t dispatched = 0;
    bool captured = false;
    while (!m_pendingCaptureEvents.empty() &&
           dispatched < kMaxCaptureEventsPerDispatch) {
        const PendingCaptureEvent event = m_pendingCaptureEvents.front();
        m_pendingCaptureEvents.pop_front();
        ++dispatched;
        captured = true;

        // Invoke a copy: callback code may replace/detach the registered
        // std::function, clear the map, or run another objective update.
        const ObjectiveCapturedCallback callback = m_capturedCallback;
        if (!callback) continue;
        try {
            callback(event.objectiveId,
                     event.capturingTeam,
                     event.previousTeam);
        } catch (...) {
            Logger::Warn("Objective capture callback failed for objective %u",
                         event.objectiveId);
        }
    }

    if (!m_pendingCaptureEvents.empty()) {
        Logger::Warn("Objective capture callback recursion exceeded %zu events; dropping remainder",
                     kMaxCaptureEventsPerDispatch);
        m_pendingCaptureEvents.clear();
    }
    if (captured) BroadcastObjectiveStates();
}

bool ObjectiveSystem::IsPlayerInZone(uint32_t playerId, const CaptureZone& zone) const {
    if (!m_server) return false;
    auto* pm = m_server->GetPlayerManager();
    if (!pm) return false;
    auto player = pm->GetPlayer(playerId);
    if (!player) return false;

    return ContainsPoint2D(zone, player->GetPosition());
}

bool ObjectiveSystem::ContainsPoint2D(const CaptureZone& zone, const Vector3& point) {
    if (!std::isfinite(zone.position.x) || !std::isfinite(zone.position.y) ||
        !std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(zone.captureRadius) || zone.captureRadius <= 0.0f) {
        return false;
    }
    const double dx = static_cast<double>(point.x) -
                      static_cast<double>(zone.position.x);
    const double dy = static_cast<double>(point.y) -
                      static_cast<double>(zone.position.y);
    // Retail uses authored volume geometry. Until those shapes are extracted,
    // the map format's documented radius is a 2D approximation in meters. Pawn
    // ServerMove locations are Unreal coordinates (50 UU per displayed meter),
    // so convert once at this boundary instead of treating 35m as 35 UU.
    const double radiusUU = static_cast<double>(zone.captureRadius) *
                            static_cast<double>(kUnrealUnitsPerMeter);
    return dx * dx + dy * dy <= radiusUU * radiusUU;
}

float ObjectiveSystem::ClampCaptureDeltaToPhase(float deltaSeconds,
                                                float phaseTimeRemaining) {
    if (!std::isfinite(deltaSeconds) || !std::isfinite(phaseTimeRemaining) ||
        deltaSeconds <= 0.0f || phaseTimeRemaining <= 0.0f) {
        return 0.0f;
    }
    return std::min(deltaSeconds, phaseTimeRemaining);
}

void ObjectiveSystem::OnPlayerEnterZone(uint32_t playerId, uint32_t objectiveId) {
    // Handled by RefreshPlayerZones in Update
}

void ObjectiveSystem::OnPlayerLeaveZone(uint32_t playerId, uint32_t objectiveId) {
    // Handled by RefreshPlayerZones in Update
}

void ObjectiveSystem::SetBotCaptureWeightProvider(
    ObjectiveBotCaptureWeightProvider provider) {
    // Cached weights belong to the provider that produced them. Clear them
    // immediately so replacing or detaching BotManager cannot leak one frame of
    // stale capture/HUD state.
    for (auto& [id, zone] : m_objectives) {
        (void)id;
        zone.botCaptureWeightByTeam.fill(0.0f);
    }
    m_botCaptureWeightProvider = std::move(provider);
    ++m_botCaptureProviderGeneration;
}

uint32_t ObjectiveSystem::GetObjectiveCount() const {
    return static_cast<uint32_t>(std::count_if(
        m_objectives.begin(), m_objectives.end(),
        [](const auto& entry) { return entry.second.enabled; }));
}

uint32_t ObjectiveSystem::GetTeamObjectiveCount(uint32_t teamId) const {
    if (!IsPlayableTeam(teamId)) return 0;
    uint32_t count = 0;
    for (const auto& [id, zone] : m_objectives) {
        if (zone.enabled && zone.controllingTeam == teamId) count++;
    }
    return count;
}

float ObjectiveSystem::GetBotCaptureWeight(uint32_t objectiveId,
                                           uint32_t serverTeamId) const {
    if (serverTeamId != TeamMapping::kServerUs &&
        serverTeamId != TeamMapping::kServerNva) {
        return 0.0f;
    }

    const CaptureZone* zone = GetObjective(objectiveId);
    if (!zone || !zone->enabled || !zone->isActive) return 0.0f;
    return SanitizeCaptureStrength(zone->botCaptureWeightByTeam[serverTeamId]);
}

bool ObjectiveSystem::AreAllObjectivesCapturedBy(uint32_t teamId) const {
    if (!IsPlayableTeam(teamId)) return false;
    if (!m_territoryOrder.empty()) {
        bool hasEnabledObjective = false;
        for (uint32_t objectiveId : m_territoryOrder) {
            const CaptureZone* zone = GetObjective(objectiveId);
            if (!zone || !zone->enabled) continue;
            hasEnabledObjective = true;
            if (zone->controllingTeam != teamId) return false;
        }
        return hasEnabledObjective;
    }

    bool hasEnabledObjective = false;
    for (const auto& [id, zone] : m_objectives) {
        (void)id;
        if (!zone.enabled) continue;
        hasEnabledObjective = true;
        if (zone.controllingTeam != teamId) return false;
    }
    return hasEnabledObjective;
}

void ObjectiveSystem::SetOnObjectiveCaptured(ObjectiveCapturedCallback cb) {
    m_capturedCallback = std::move(cb);
}

void ObjectiveSystem::BroadcastObjectiveStates() const {
    if (!m_server) return;
    if (auto* network = m_server->GetNetworkManager()) {
        // Retail clients consume the live GRI array state; ClientConnection
        // suppresses the legacy packet format for those peers.
        network->BroadcastRetailObjectiveState();
    }

    std::vector<uint8_t> data;
    uint32_t count = static_cast<uint32_t>(m_objectives.size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&count),
                reinterpret_cast<uint8_t*>(&count) + sizeof(count));

    for (const auto& [id, zone] : m_objectives) {
        // id, controllingTeam, state, captureProgress, isActive
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.id),
                    reinterpret_cast<const uint8_t*>(&zone.id) + sizeof(zone.id));
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.controllingTeam),
                    reinterpret_cast<const uint8_t*>(&zone.controllingTeam) + sizeof(zone.controllingTeam));
        uint8_t stateVal = static_cast<uint8_t>(zone.state);
        data.push_back(stateVal);
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&zone.captureProgress),
                    reinterpret_cast<const uint8_t*>(&zone.captureProgress) + sizeof(zone.captureProgress));
        data.push_back(zone.enabled && zone.isActive ? 1 : 0);
    }

    if (auto* network = m_server->GetNetworkManager()) {
        network->BroadcastPacket("OBJECTIVE_UPDATE", data);
    }
}
