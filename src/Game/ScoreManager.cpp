// src/Game/ScoreManager.cpp – Implementation for ScoreManager

#include "Game/ScoreManager.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include <algorithm>

ScoreManager::ScoreManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ScoreManager::ScoreManager] Entry, server=%p", static_cast<void*>(server));
    m_teamManager = m_server->GetTeamManager();
    Logger::Debug("[ScoreManager::ScoreManager] TeamManager obtained: %p", static_cast<void*>(m_teamManager));
    Logger::Trace("[ScoreManager::ScoreManager] Exit");
}

ScoreManager::~ScoreManager() {
    Logger::Trace("[ScoreManager::~ScoreManager] Entry");
    Logger::Trace("[ScoreManager::~ScoreManager] Exit");
}

void ScoreManager::Initialize() {
    Logger::Trace("[ScoreManager::Initialize] Entry");
    Logger::Info("ScoreManager initialized");
    ResetAll();
    Logger::Trace("[ScoreManager::Initialize] Exit");
}

void ScoreManager::Shutdown() {
    Logger::Trace("[ScoreManager::Shutdown] Entry");
    Logger::Info("ScoreManager shutdown");
    Logger::Debug("[ScoreManager::Shutdown] Clearing %zu team scores", m_scores.size());
    m_scores.clear();
    Logger::Trace("[ScoreManager::Shutdown] Exit");
}

void ScoreManager::EnsureTeamExists(uint32_t teamId) {
    Logger::Trace("[ScoreManager::EnsureTeamExists] Entry, teamId=%u", teamId);
    if (m_scores.find(teamId) == m_scores.end()) {
        m_scores[teamId] = TeamScore{};
        Logger::Debug("[ScoreManager::EnsureTeamExists] Created new TeamScore entry for team %u", teamId);
    } else {
        Logger::Debug("[ScoreManager::EnsureTeamExists] Team %u already exists", teamId);
    }
    Logger::Trace("[ScoreManager::EnsureTeamExists] Exit");
}

void ScoreManager::RecordKill(uint32_t teamId) {
    Logger::Trace("[ScoreManager::RecordKill] Entry, teamId=%u", teamId);
    EnsureTeamExists(teamId);
    m_scores[teamId].kills++;
    m_scores[teamId].score += 1;  // 1 point per kill
    Logger::Debug("Team %u kill recorded (total kills=%u)", teamId, m_scores[teamId].kills);
    Logger::Debug("[ScoreManager::RecordKill] Team %u score now %u after kill bonus", teamId, m_scores[teamId].score);
    BroadcastScores();
    Logger::Trace("[ScoreManager::RecordKill] Exit");
}

void ScoreManager::RecordDeath(uint32_t teamId) {
    Logger::Trace("[ScoreManager::RecordDeath] Entry, teamId=%u", teamId);
    EnsureTeamExists(teamId);
    m_scores[teamId].deaths++;
    Logger::Debug("Team %u death recorded (total deaths=%u)", teamId, m_scores[teamId].deaths);
    BroadcastScores();
    Logger::Trace("[ScoreManager::RecordDeath] Exit");
}

void ScoreManager::RecordObjectiveCapture(uint32_t teamId, uint32_t points) {
    Logger::Trace("[ScoreManager::RecordObjectiveCapture] Entry, teamId=%u, points=%u", teamId, points);
    EnsureTeamExists(teamId);
    m_scores[teamId].objectivesCaptured++;
    m_scores[teamId].score += points;
    Logger::Info("Team %u captured objective (+%u points)", teamId, points);
    Logger::Debug("[ScoreManager::RecordObjectiveCapture] Team %u now has %u objectives captured, total score=%u",
                  teamId, m_scores[teamId].objectivesCaptured, m_scores[teamId].score);
    BroadcastScores();
    Logger::Trace("[ScoreManager::RecordObjectiveCapture] Exit");
}

void ScoreManager::AddPoints(uint32_t teamId, uint32_t points) {
    Logger::Trace("[ScoreManager::AddPoints] Entry, teamId=%u, points=%u", teamId, points);
    EnsureTeamExists(teamId);
    uint32_t oldScore = m_scores[teamId].score;
    m_scores[teamId].score += points;
    Logger::Debug("Team %u awarded %u points (total=%u)", teamId, points, m_scores[teamId].score);
    Logger::Debug("[ScoreManager::AddPoints] Team %u score changed: %u -> %u", teamId, oldScore, m_scores[teamId].score);
    BroadcastScores();
    Logger::Trace("[ScoreManager::AddPoints] Exit");
}

void ScoreManager::SetPoints(uint32_t teamId, uint32_t points) {
    Logger::Trace("[ScoreManager::SetPoints] Entry, teamId=%u, points=%u", teamId, points);
    EnsureTeamExists(teamId);
    uint32_t oldScore = m_scores[teamId].score;
    m_scores[teamId].score = points;
    Logger::Debug("Team %u score set to %u", teamId, points);
    Logger::Debug("[ScoreManager::SetPoints] Team %u score overridden: %u -> %u", teamId, oldScore, points);
    BroadcastScores();
    Logger::Trace("[ScoreManager::SetPoints] Exit");
}

TeamScore ScoreManager::GetTeamScore(uint32_t teamId) const {
    Logger::Trace("[ScoreManager::GetTeamScore] Entry, teamId=%u", teamId);
    auto it = m_scores.find(teamId);
    if (it != m_scores.end()) {
        Logger::Debug("[ScoreManager::GetTeamScore] Found score for team %u: score=%u, kills=%u, deaths=%u",
                      teamId, it->second.score, it->second.kills, it->second.deaths);
        Logger::Trace("[ScoreManager::GetTeamScore] Exit, return valid TeamScore");
        return it->second;
    }
    Logger::Debug("[ScoreManager::GetTeamScore] No score found for team %u, returning default", teamId);
    Logger::Trace("[ScoreManager::GetTeamScore] Exit, return default TeamScore");
    return TeamScore{};
}

std::vector<uint32_t> ScoreManager::GetTeamsByScore() const {
    Logger::Trace("[ScoreManager::GetTeamsByScore] Entry");
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto& [teamId, score] : m_scores) {
        entries.emplace_back(teamId, score.score);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto& a, auto& b){ return a.second > b.second; });
    std::vector<uint32_t> ordered;
    for (auto& e : entries) ordered.push_back(e.first);
    Logger::Debug("[ScoreManager::GetTeamsByScore] Sorted %zu teams by score", ordered.size());
    Logger::Trace("[ScoreManager::GetTeamsByScore] Exit, return %zu teams", ordered.size());
    return ordered;
}

void ScoreManager::ResetAll() {
    Logger::Trace("[ScoreManager::ResetAll] Entry");
    m_scores.clear();
    if (m_teamManager) {
        uint32_t teamCount = m_teamManager->GetTeamCount();
        Logger::Debug("[ScoreManager::ResetAll] Initializing scores for %u teams", teamCount);
        for (uint32_t teamId = 1; teamId <= teamCount; ++teamId) {
            m_scores[teamId] = TeamScore{};
            Logger::Debug("[ScoreManager::ResetAll] Team %u score entry created", teamId);
        }
    } else {
        Logger::Warn("[ScoreManager::ResetAll] TeamManager is null, cannot initialize team scores");
    }
    Logger::Info("ScoreManager: All scores reset");
    BroadcastScores();
    Logger::Trace("[ScoreManager::ResetAll] Exit");
}

void ScoreManager::BroadcastScores() const {
    Logger::Trace("[ScoreManager::BroadcastScores] Entry");
    // Create simple score packet: teamId,score pairs
    std::vector<uint8_t> data;
    uint32_t count = static_cast<uint32_t>(m_scores.size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&count), reinterpret_cast<uint8_t*>(&count) + sizeof(count));
    for (const auto& [teamId, ts] : m_scores) {
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&teamId), reinterpret_cast<const uint8_t*>(&teamId) + sizeof(teamId));
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&ts.score), reinterpret_cast<const uint8_t*>(&ts.score) + sizeof(ts.score));
    }
    m_server->GetNetworkManager()->BroadcastPacket("SCORE_UPDATE", data);
    Logger::Debug("Broadcasted scores for %u teams", count);
    Logger::Trace("[ScoreManager::BroadcastScores] Exit");
}
