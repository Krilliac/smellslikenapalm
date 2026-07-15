// src/Game/SupremacyMode.cpp
// RS2V Supremacy game mode implementation.

#include "Game/SupremacyMode.h"

#include "Game/BotManager.h"
#include "Game/GameServer.h"
#include "Game/ObjectiveSystem.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <limits>

SupremacyMode::SupremacyMode(GameServer* server)
    : m_server(server)
{
}

SupremacyMode::~SupremacyMode() {
    Shutdown();
}

void SupremacyMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_score = 0;
    m_winningTeam = 0;
    m_scoreTickTimer = 0.0;
    ResetObjectivesToInitialOwners();
    Logger::Info("SupremacyMode initialized (signed target +/-%d)", m_scoreTarget);
}

void SupremacyMode::Shutdown() {
    Logger::Info("SupremacyMode shutdown");
}

void SupremacyMode::StartRound() {
    m_score = 0;
    m_winningTeam = 0;
    m_scoreTickTimer = 0.0;
    ResetObjectivesToInitialOwners();
    if (m_server) {
        if (auto* objectives = m_server->GetObjectiveSystem()) {
            objectives->ResetObjectivesToInitialOwners();
        }
    }
    SetPhase(Phase::Preparation);
    Logger::Info("Supremacy round started");
}

void SupremacyMode::EndRound() {
    FinishRoundWithWinner(DetermineWinner());
}

void SupremacyMode::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return;

    const float phaseTimeBeforeUpdate = m_phaseTimer;
    m_phaseTimer = std::max(0.0f, m_phaseTimer - deltaSeconds);

    switch (m_phase) {
        case Phase::WarmUp:
            if (m_server && m_server->GetTeamManager() &&
                m_server->GetTeamManager()->HasEnoughPlayers()) {
                StartRound();
            }
            break;

        case Phase::Preparation:
            if (m_phaseTimer <= 0.0f) {
                SetPhase(Phase::Active);
            }
            break;

        case Phase::Active:
            // A delayed frame may extend beyond the round deadline. Only the
            // portion that actually occurred while Active contributes score;
            // this makes one large update equivalent to updates split exactly
            // at the phase boundary.
            ProcessScoreFlow(std::min(
                deltaSeconds, std::max(0.0f, phaseTimeBeforeUpdate)));
            CheckWinConditions();
            if (m_phaseTimer <= 0.0f && m_phase == Phase::Active) {
                EndRound();
            }
            break;

        case Phase::SuddenDeath:
            // Bot deaths do not have client ids and therefore do not travel
            // through OnPlayerKilled. Poll the unified liveness view while
            // respawns are disabled so a headless final kill resolves.
            CheckSuddenDeathElimination();
            break;

        case Phase::PostRound:
            if (m_phaseTimer <= 0.0f) {
                SetPhase(Phase::Finished);
            }
            break;

        case Phase::Finished:
            break;
    }
}

void SupremacyMode::ProcessScoreFlow(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;

    m_scoreTickTimer += static_cast<double>(deltaSeconds);
    const double elapsedTicks =
        std::floor(m_scoreTickTimer / static_cast<double>(m_scoringInterval));
    if (elapsedTicks < 1.0) return;

    m_scoreTickTimer = std::fmod(
        m_scoreTickTimer, static_cast<double>(m_scoringInterval));

    const int64_t southConnectedValue =
        CalculateLinkedObjectiveValue(kSouthTeamId);
    const int64_t northConnectedValue =
        CalculateLinkedObjectiveValue(kNorthTeamId);
    const int64_t scorePerTick = southConnectedValue - northConnectedValue;
    if (scorePerTick == 0) return;

    // Decide whether the accumulated ticks reach a target before converting
    // the double tick count. If they do not, the exact int64 product is known
    // to be smaller than the at-most-2*INT32_MAX distance to that target. This
    // avoids both a narrowing conversion of a large catch-up delta and signed
    // overflow when adding it to a score near the opposite bound.
    const int64_t currentScore = static_cast<int64_t>(m_score);
    const int64_t target = static_cast<int64_t>(m_scoreTarget);
    int64_t finalScore = currentScore;

    if (scorePerTick > 0) {
        const int64_t distance = target - currentScore;
        const int64_t ticksToTarget =
            distance / scorePerTick + (distance % scorePerTick != 0 ? 1 : 0);
        if (elapsedTicks >= static_cast<double>(ticksToTarget)) {
            finalScore = target;
        } else {
            const int64_t tickCount = static_cast<int64_t>(elapsedTicks);
            finalScore = currentScore + scorePerTick * tickCount;
        }
    } else {
        // CalculateLinkedObjectiveValue is non-negative and saturates at
        // INT64_MAX, so scorePerTick can never be INT64_MIN.
        const int64_t scorePerTickMagnitude = -scorePerTick;
        const int64_t distance = currentScore + target;
        const int64_t ticksToTarget =
            distance / scorePerTickMagnitude +
            (distance % scorePerTickMagnitude != 0 ? 1 : 0);
        if (elapsedTicks >= static_cast<double>(ticksToTarget)) {
            finalScore = -target;
        } else {
            const int64_t tickCount = static_cast<int64_t>(elapsedTicks);
            finalScore = currentScore - scorePerTickMagnitude * tickCount;
        }
    }

    finalScore = std::clamp(finalScore, -target, target);
    m_score = static_cast<int32_t>(finalScore);
}

bool SupremacyMode::UsesSupplyLines() const {
    // ROGameInfoSupremacy.Reset enables bUseSupplyLines only after it finds one
    // distinct HomeBaseForTeam marker for each team. A marker necessarily
    // belongs to an objective in retail; reject API-created dangling aliases.
    if (!m_southHQ || !m_northHQ || *m_southHQ == *m_northHQ) {
        return false;
    }

    return m_objectiveGraph.find(*m_southHQ) != m_objectiveGraph.end() &&
           m_objectiveGraph.find(*m_northHQ) != m_objectiveGraph.end();
}

int64_t SupremacyMode::CalculateLinkedObjectiveValue(uint32_t teamId) const {
    if (teamId != kSouthTeamId && teamId != kNorthTeamId) return 0;

    const bool useSupplyLines = UsesSupplyLines();
    int64_t total = 0;
    for (const auto& [id, node] : m_objectiveGraph) {
        if (node.controllingTeam != teamId) continue;

        if (useSupplyLines && !HasPathFromHQ(id, teamId)) continue;

        const int64_t pointValue = static_cast<int64_t>(node.pointValue);
        if (total > std::numeric_limits<int64_t>::max() - pointValue) {
            total = std::numeric_limits<int64_t>::max();
        } else {
            total += pointValue;
        }
    }
    return total;
}

bool SupremacyMode::HasPathFromHQ(uint32_t objectiveId,
                                  uint32_t teamId) const {
    const auto objectiveIt = m_objectiveGraph.find(objectiveId);
    if (objectiveIt == m_objectiveGraph.end() ||
        objectiveIt->second.controllingTeam != teamId) {
        return false;
    }

    // ROGameInfoSupremacy.UpdatePointsHeld explicitly marks objectives with no
    // bordering objectives as connected. This applies even when the team's HQ
    // is currently enemy-controlled.
    if (objectiveIt->second.linkedTo.empty()) return true;

    const auto hq = GetTeamHQ(teamId);
    if (!hq) return false;

    const auto hqIt = m_objectiveGraph.find(*hq);
    if (hqIt == m_objectiveGraph.end() ||
        hqIt->second.controllingTeam != teamId) {
        return false;
    }

    // Retail starts at the owned home base and follows BorderingObjectives
    // outward. Do not reverse the authored edges by searching from the target.
    std::vector<uint32_t> pending{*hq};
    std::vector<uint32_t> visited;
    while (!pending.empty()) {
        const uint32_t current = pending.back();
        pending.pop_back();

        if (std::find(visited.begin(), visited.end(), current) !=
            visited.end()) {
            continue;
        }
        visited.push_back(current);
        if (current == objectiveId) return true;

        const auto currentIt = m_objectiveGraph.find(current);
        if (currentIt == m_objectiveGraph.end() ||
            currentIt->second.controllingTeam != teamId) {
            continue;
        }

        for (uint32_t linked : currentIt->second.linkedTo) {
            const auto linkedIt = m_objectiveGraph.find(linked);
            if (linkedIt != m_objectiveGraph.end() &&
                linkedIt->second.controllingTeam == teamId) {
                pending.push_back(linked);
            }
        }
    }
    return false;
}

void SupremacyMode::OnObjectiveCaptured(uint32_t objectiveId,
                                        uint32_t capturingTeam) {
    if (m_phase != Phase::Active ||
        (capturingTeam != kSouthTeamId && capturingTeam != kNorthTeamId)) {
        return;
    }

    auto it = m_objectiveGraph.find(objectiveId);
    if (it == m_objectiveGraph.end()) return;
    if (it->second.controllingTeam == capturingTeam) return;
    it->second.controllingTeam = capturingTeam;
    Logger::Info("Supremacy objective %u captured by team %u",
                 objectiveId, capturingTeam);
}

void SupremacyMode::OnTicketsDepleted(uint32_t teamId) {
    if (m_phase != Phase::Active ||
        (teamId != kSouthTeamId && teamId != kNorthTeamId)) {
        return;
    }
    Logger::Info("Team %u tickets depleted in Supremacy - sudden death",
                 teamId);
    SetPhase(Phase::SuddenDeath);
}

void SupremacyMode::OnPlayerKilled(uint32_t /*killerId*/,
                                   uint32_t /*victimId*/) {
    CheckSuddenDeathElimination();
}

void SupremacyMode::OnTeamEliminated(uint32_t eliminatedTeamId) {
    if (m_phase != Phase::SuddenDeath ||
        (eliminatedTeamId != kSouthTeamId &&
         eliminatedTeamId != kNorthTeamId)) {
        return;
    }

    // A human-only roster notification is not an authoritative elimination
    // while this team still has a living headless participant.
    if (m_server && TeamHasLivingParticipant(eliminatedTeamId)) {
        Logger::Info("Ignoring Supremacy team %u elimination; participants remain alive",
                     eliminatedTeamId);
        return;
    }

    const uint32_t winningTeam = eliminatedTeamId == kSouthTeamId
        ? kNorthTeamId
        : kSouthTeamId;
    Logger::Info("Team %u eliminated in Supremacy sudden death; team %u wins",
                 eliminatedTeamId, winningTeam);
    FinishRoundWithWinner(winningTeam);
}

float SupremacyMode::GetRoundTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float SupremacyMode::GetPhaseDuration() const {
    switch (m_phase) {
        case Phase::WarmUp:      return 0.0f;
        case Phase::Preparation: return m_preparationTime;
        case Phase::Active:      return m_roundTime;
        case Phase::SuddenDeath: return 0.0f;
        case Phase::PostRound:   return m_postRoundTime;
        case Phase::Finished:    return 0.0f;
    }
    return 0.0f;
}

float SupremacyMode::GetPhaseTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float SupremacyMode::GetTeamPoints(uint32_t teamId) const {
    if (teamId == kSouthTeamId) {
        return static_cast<float>(
            (static_cast<double>(m_scoreTarget) + static_cast<double>(m_score)) *
            0.5);
    }
    if (teamId == kNorthTeamId) {
        return static_cast<float>(
            (static_cast<double>(m_scoreTarget) - static_cast<double>(m_score)) *
            0.5);
    }
    return 0.0f;
}

float SupremacyMode::GetPointBarProgress() const {
    return static_cast<float>(
        (static_cast<double>(m_scoreTarget) + static_cast<double>(m_score)) /
        (2.0 * static_cast<double>(m_scoreTarget)));
}

int SupremacyMode::GetTeamObjectiveValue(uint32_t teamId) const {
    return static_cast<int>(std::min(
        CalculateLinkedObjectiveValue(teamId),
        static_cast<int64_t>(std::numeric_limits<int>::max())));
}

int SupremacyMode::GetSouthConnectedObjectiveValue() const {
    return GetTeamObjectiveValue(kSouthTeamId);
}

int SupremacyMode::GetNorthConnectedObjectiveValue() const {
    return GetTeamObjectiveValue(kNorthTeamId);
}

bool SupremacyMode::IsObjectiveLinked(uint32_t objectiveId,
                                      uint32_t teamId) const {
    if (!UsesSupplyLines()) return false;
    return HasPathFromHQ(objectiveId, teamId);
}

bool SupremacyMode::HasObjective(uint32_t objectiveId) const {
    return m_objectiveGraph.find(objectiveId) != m_objectiveGraph.end();
}

uint32_t SupremacyMode::GetObjectiveControllingTeam(
    uint32_t objectiveId) const {
    const auto it = m_objectiveGraph.find(objectiveId);
    return it == m_objectiveGraph.end() ? 0 : it->second.controllingTeam;
}

int SupremacyMode::GetObjectivePointValue(uint32_t objectiveId) const {
    const auto it = m_objectiveGraph.find(objectiveId);
    return it == m_objectiveGraph.end() ? 0 : it->second.pointValue;
}

std::vector<uint32_t> SupremacyMode::GetObjectiveLinks(
    uint32_t objectiveId) const {
    const auto it = m_objectiveGraph.find(objectiveId);
    return it == m_objectiveGraph.end()
               ? std::vector<uint32_t>{}
               : it->second.linkedTo;
}

std::optional<uint32_t> SupremacyMode::GetTeamHQ(
    uint32_t teamId) const {
    if (teamId == kSouthTeamId) return m_southHQ;
    if (teamId == kNorthTeamId) return m_northHQ;
    return std::nullopt;
}

void SupremacyMode::SetScoreTarget(int32_t target) {
    if (target <= 0) return;
    m_scoreTarget = target;
    m_score = std::clamp(m_score, -m_scoreTarget, m_scoreTarget);
}

void SupremacyMode::SetScoringInterval(float seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0f) return;
    m_scoringInterval = std::max(0.01f, seconds);
    m_scoreTickTimer = std::fmod(
        m_scoreTickTimer, static_cast<double>(m_scoringInterval));
}

void SupremacyMode::SetStartingPoints(float points) {
    if (!std::isfinite(points) || points <= 0.0f) return;

    const double doubled = std::min(
        static_cast<double>(points) * 2.0,
        static_cast<double>(std::numeric_limits<int32_t>::max()));
    SetScoreTarget(static_cast<int32_t>(std::lround(doubled)));
}

void SupremacyMode::SetPointDrainInterval(float seconds) {
    SetScoringInterval(seconds);
}

void SupremacyMode::SetRoundTime(float seconds) {
    if (std::isfinite(seconds) && seconds > 0.0f) {
        m_roundTime = seconds;
    }
}

void SupremacyMode::ClearObjectives() {
    m_objectiveGraph.clear();
    m_southHQ.reset();
    m_northHQ.reset();
}

void SupremacyMode::SetObjectiveMetadata(uint32_t objectiveId,
                                         uint32_t controllingTeam,
                                         int pointValue) {
    auto& node = m_objectiveGraph[objectiveId];
    node.id = objectiveId;
    node.pointValue = std::max(0, pointValue);
    node.initialControllingTeam =
        controllingTeam <= kNorthTeamId ? controllingTeam : 0;
    node.controllingTeam = node.initialControllingTeam;
}

void SupremacyMode::SetObjectiveLinks(
    const std::map<uint32_t, std::vector<uint32_t>>& links) {
    for (auto& [id, node] : m_objectiveGraph) {
        (void)id;
        node.linkedTo.clear();
    }

    for (const auto& [id, linkedIds] : links) {
        m_objectiveGraph[id].id = id;
        m_objectiveGraph[id].linkedTo = linkedIds;
        for (uint32_t linkedId : linkedIds) {
            m_objectiveGraph[linkedId].id = linkedId;
        }
    }
}

void SupremacyMode::SetTeamHQ(uint32_t teamId, uint32_t objectiveId) {
    if (teamId == kSouthTeamId) {
        m_southHQ = objectiveId;
    } else if (teamId == kNorthTeamId) {
        m_northHQ = objectiveId;
    }
}

void SupremacyMode::ResetObjectivesToInitialOwners() {
    for (auto& [id, node] : m_objectiveGraph) {
        (void)id;
        node.controllingTeam = node.initialControllingTeam;
    }
}

bool SupremacyMode::TeamHasLivingParticipant(uint32_t teamId) const {
    if (!m_server ||
        (teamId != kSouthTeamId && teamId != kNorthTeamId)) {
        return false;
    }

    auto* players = m_server->GetPlayerManager();
    auto* teams = m_server->GetTeamManager();
    if (players && teams) {
        for (const uint32_t playerId : teams->GetTeamPlayers(teamId)) {
            const auto player = players->GetPlayer(playerId);
            if (player && player->IsAlive()) return true;
        }
    }

    if (const auto* bots = m_server->GetBotManager()) {
        return bots->CountAliveBots(static_cast<uint8_t>(teamId)) > 0;
    }
    return false;
}

void SupremacyMode::CheckSuddenDeathElimination() {
    if (m_phase != Phase::SuddenDeath || !m_server) return;

    const bool southAlive = TeamHasLivingParticipant(kSouthTeamId);
    const bool northAlive = TeamHasLivingParticipant(kNorthTeamId);
    if (!southAlive && !northAlive) {
        Logger::Info("Both teams eliminated in Supremacy sudden death");
        FinishRoundWithWinner(0);
    } else if (!southAlive) {
        OnTeamEliminated(kSouthTeamId);
    } else if (!northAlive) {
        OnTeamEliminated(kNorthTeamId);
    }
}

void SupremacyMode::SetPhase(Phase newPhase) {
    m_phase = newPhase;
    switch (newPhase) {
        case Phase::WarmUp:
            m_phaseTimer = 0.0f;
            break;
        case Phase::Preparation:
            m_phaseTimer = m_preparationTime;
            break;
        case Phase::Active:
            m_phaseTimer = m_roundTime;
            break;
        case Phase::SuddenDeath:
            m_phaseTimer = 0.0f;
            break;
        case Phase::PostRound:
            m_phaseTimer = m_postRoundTime;
            break;
        case Phase::Finished:
            m_phaseTimer = 0.0f;
            break;
    }
    BroadcastPhaseChange();
}

void SupremacyMode::CheckWinConditions() {
    if (m_score <= -m_scoreTarget) {
        Logger::Info("North wins Supremacy (score %d)", m_score);
        EndRound();
    } else if (m_score >= m_scoreTarget) {
        Logger::Info("South wins Supremacy (score %+d)", m_score);
        EndRound();
    }
}

uint32_t SupremacyMode::DetermineWinner() const {
    if (m_score > 0) {
        return kSouthTeamId;
    }
    if (m_score < 0) {
        return kNorthTeamId;
    }
    return 0;
}

void SupremacyMode::FinishRoundWithWinner(uint32_t winningTeam) {
    if (winningTeam > kNorthTeamId ||
        (m_phase != Phase::Active && m_phase != Phase::SuddenDeath)) {
        return;
    }

    m_winningTeam = winningTeam;
    if (m_winningTeam == kSouthTeamId) {
        Logger::Info("South wins Supremacy (score %+d)", m_score);
    } else if (m_winningTeam == kNorthTeamId) {
        Logger::Info("North wins Supremacy (score %d)", m_score);
    } else {
        Logger::Info("Supremacy draw (score %+d)", m_score);
    }

    SetPhase(Phase::PostRound);
}

void SupremacyMode::BroadcastPhaseChange() const {
    std::string message;
    switch (m_phase) {
        case Phase::WarmUp:
            message = "Waiting for players...";
            break;
        case Phase::Preparation:
            message = "Round starting soon!";
            break;
        case Phase::Active:
            message = "Fight for objectives!";
            break;
        case Phase::SuddenDeath:
            message = "SUDDEN DEATH - No respawns!";
            break;
        case Phase::PostRound:
            message = "Round over!";
            break;
        case Phase::Finished:
            message = "Match complete!";
            break;
    }

    if (m_server) {
        m_server->BroadcastChatMessage("[Supremacy] " + message);
    }
}
