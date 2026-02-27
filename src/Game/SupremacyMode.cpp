// src/Game/SupremacyMode.cpp
// RS2V Supremacy game mode implementation

#include "Game/SupremacyMode.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include <algorithm>

SupremacyMode::SupremacyMode(GameServer* server)
    : m_server(server)
{
}

SupremacyMode::~SupremacyMode() {
    Shutdown();
}

void SupremacyMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_team1Points = m_startingPoints;
    m_team2Points = m_startingPoints;
    m_drainTimer = 0.0f;
    Logger::Info("SupremacyMode initialized (%.0f starting points per team)", m_startingPoints);
}

void SupremacyMode::Shutdown() {
    Logger::Info("SupremacyMode shutdown");
}

void SupremacyMode::StartRound() {
    m_team1Points = m_startingPoints;
    m_team2Points = m_startingPoints;
    m_drainTimer = 0.0f;
    SetPhase(Phase::Preparation);
    Logger::Info("Supremacy round started");
}

void SupremacyMode::EndRound() {
    DetermineWinner();
    SetPhase(Phase::PostRound);
}

void SupremacyMode::Update(float deltaSeconds) {
    m_phaseTimer -= deltaSeconds;

    switch (m_phase) {
        case Phase::WarmUp:
            if (m_server->GetTeamManager()->HasEnoughPlayers()) {
                StartRound();
            }
            break;

        case Phase::Preparation:
            if (m_phaseTimer <= 0.0f) {
                SetPhase(Phase::Active);
            }
            break;

        case Phase::Active:
            ProcessPointDrain(deltaSeconds);
            CheckWinConditions();
            if (m_phaseTimer <= 0.0f) {
                EndRound();
            }
            break;

        case Phase::SuddenDeath:
            // No point drain, wait for elimination
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

void SupremacyMode::ProcessPointDrain(float deltaSeconds) {
    m_drainTimer += deltaSeconds;
    if (m_drainTimer < m_pointDrainInterval) return;
    m_drainTimer -= m_pointDrainInterval;

    // Calculate linked objective value for each team
    int team1Value = CalculateLinkedObjectiveValue(1);
    int team2Value = CalculateLinkedObjectiveValue(2);

    int diff = team1Value - team2Value;

    if (diff > 0) {
        // Team 1 has more connected objective value — drain from team 2
        float drain = static_cast<float>(diff);
        m_team2Points -= drain;
        m_team1Points += drain;
    } else if (diff < 0) {
        // Team 2 has more
        float drain = static_cast<float>(-diff);
        m_team1Points -= drain;
        m_team2Points += drain;
    }

    m_team1Points = std::max(0.0f, m_team1Points);
    m_team2Points = std::max(0.0f, m_team2Points);
}

int SupremacyMode::CalculateLinkedObjectiveValue(uint32_t teamId) const {
    int total = 0;
    uint32_t hq = (teamId == 1) ? m_team1HQ : m_team2HQ;

    // Check if team still controls their HQ
    auto hqIt = m_objectiveGraph.find(hq);
    if (hqIt == m_objectiveGraph.end() || hqIt->second.controllingTeam != teamId) {
        return 0;  // Lost HQ — zero points
    }

    // BFS/DFS to find all objectives connected back to HQ through team-controlled nodes
    for (const auto& [id, node] : m_objectiveGraph) {
        if (node.controllingTeam != teamId) continue;

        std::vector<uint32_t> visited;
        if (HasPathToHQ(id, teamId, visited)) {
            total += node.pointValue;
        }
    }
    return total;
}

bool SupremacyMode::HasPathToHQ(uint32_t objectiveId, uint32_t teamId,
                                  std::vector<uint32_t>& visited) const {
    uint32_t hq = (teamId == 1) ? m_team1HQ : m_team2HQ;
    if (objectiveId == hq) return true;

    visited.push_back(objectiveId);

    auto it = m_objectiveGraph.find(objectiveId);
    if (it == m_objectiveGraph.end()) return false;

    for (uint32_t linked : it->second.linkedTo) {
        if (std::find(visited.begin(), visited.end(), linked) != visited.end()) continue;

        auto linkIt = m_objectiveGraph.find(linked);
        if (linkIt == m_objectiveGraph.end()) continue;
        if (linkIt->second.controllingTeam != teamId) continue;

        if (HasPathToHQ(linked, teamId, visited)) return true;
    }
    return false;
}

void SupremacyMode::OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam) {
    auto it = m_objectiveGraph.find(objectiveId);
    if (it != m_objectiveGraph.end()) {
        it->second.controllingTeam = capturingTeam;
    }
    Logger::Info("Supremacy objective %u captured by team %u", objectiveId, capturingTeam);
}

void SupremacyMode::OnTicketsDepleted(uint32_t teamId) {
    Logger::Info("Team %u tickets depleted in Supremacy — sudden death", teamId);
    SetPhase(Phase::SuddenDeath);
}

void SupremacyMode::OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {
    // Check sudden death elimination
    if (m_phase == Phase::SuddenDeath) {
        auto* pm = m_server->GetPlayerManager();
        for (uint32_t teamId = 1; teamId <= 2; teamId++) {
            bool anyAlive = false;
            for (uint32_t pid : m_server->GetTeamManager()->GetTeamPlayers(teamId)) {
                auto p = pm->GetPlayer(pid);
                if (p && p->IsAlive()) { anyAlive = true; break; }
            }
            if (!anyAlive) {
                Logger::Info("Team %u eliminated in sudden death", teamId);
                EndRound();
                return;
            }
        }
    }
}

float SupremacyMode::GetRoundTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float SupremacyMode::GetTeamPoints(uint32_t teamId) const {
    return teamId == 1 ? m_team1Points : m_team2Points;
}

float SupremacyMode::GetPointBarProgress() const {
    float total = m_team1Points + m_team2Points;
    if (total <= 0.0f) return 0.5f;
    return m_team2Points / total;  // 0=team2 depleted, 1=team1 depleted
}

int SupremacyMode::GetTeamObjectiveValue(uint32_t teamId) const {
    return CalculateLinkedObjectiveValue(teamId);
}

bool SupremacyMode::IsObjectiveLinked(uint32_t objectiveId, uint32_t teamId) const {
    std::vector<uint32_t> visited;
    return HasPathToHQ(objectiveId, teamId, visited);
}

void SupremacyMode::SetStartingPoints(float points) { m_startingPoints = points; }
void SupremacyMode::SetPointDrainInterval(float seconds) { m_pointDrainInterval = seconds; }
void SupremacyMode::SetRoundTime(float seconds) { m_roundTime = seconds; }

void SupremacyMode::SetObjectiveLinks(const std::map<uint32_t, std::vector<uint32_t>>& links) {
    for (const auto& [id, linkedIds] : links) {
        m_objectiveGraph[id].id = id;
        m_objectiveGraph[id].linkedTo = linkedIds;
    }
}

void SupremacyMode::SetTeamHQ(uint32_t teamId, uint32_t objectiveId) {
    if (teamId == 1) m_team1HQ = objectiveId;
    else m_team2HQ = objectiveId;
}

void SupremacyMode::SetPhase(Phase newPhase) {
    m_phase = newPhase;
    switch (newPhase) {
        case Phase::WarmUp:      m_phaseTimer = 0.0f; break;
        case Phase::Preparation: m_phaseTimer = m_preparationTime; break;
        case Phase::Active:      m_phaseTimer = m_roundTime; break;
        case Phase::SuddenDeath: m_phaseTimer = 0.0f; break;
        case Phase::PostRound:   m_phaseTimer = m_postRoundTime; break;
        case Phase::Finished:    m_phaseTimer = 0.0f; break;
    }
    BroadcastPhaseChange();
}

void SupremacyMode::CheckWinConditions() {
    if (m_team1Points <= 0.0f) {
        Logger::Info("Team 2 wins Supremacy — Team 1 points depleted");
        EndRound();
    } else if (m_team2Points <= 0.0f) {
        Logger::Info("Team 1 wins Supremacy — Team 2 points depleted");
        EndRound();
    }
}

void SupremacyMode::DetermineWinner() {
    if (m_team1Points > m_team2Points) {
        Logger::Info("Team 1 wins Supremacy (%.0f vs %.0f points)", m_team1Points, m_team2Points);
    } else if (m_team2Points > m_team1Points) {
        Logger::Info("Team 2 wins Supremacy (%.0f vs %.0f points)", m_team2Points, m_team1Points);
    } else {
        Logger::Info("Supremacy draw (%.0f points each)", m_team1Points);
    }
    m_server->BroadcastChatMessage("[Supremacy] Match complete!");
}

void SupremacyMode::BroadcastPhaseChange() const {
    std::string msg;
    switch (m_phase) {
        case Phase::WarmUp:      msg = "Waiting for players..."; break;
        case Phase::Preparation: msg = "Round starting soon!"; break;
        case Phase::Active:      msg = "Fight for objectives!"; break;
        case Phase::SuddenDeath: msg = "SUDDEN DEATH — No respawns!"; break;
        case Phase::PostRound:   msg = "Round over!"; break;
        case Phase::Finished:    msg = "Match complete!"; break;
    }
    m_server->BroadcastChatMessage("[Supremacy] " + msg);
}
