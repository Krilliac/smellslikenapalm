// src/Game/ScoreManager.cpp – Implementation for ScoreManager

#include "Game/ScoreManager.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"

ScoreManager::ScoreManager(GameServer* server)
    : m_server(server)
{
    m_teamManager = m_server->GetTeamManager();
}

ScoreManager::~ScoreManager() = default;

void ScoreManager::Initialize() {
    Logger::Info("ScoreManager initialized");
    ResetAll();
}

void ScoreManager::Shutdown() {
    Logger::Info("ScoreManager shutdown");
    m_scores.clear();
}

void ScoreManager::EnsureTeamExists(uint32_t teamId) {
    if (m_scores.find(teamId) == m_scores.end()) {
        m_scores[teamId] = TeamScore{};
    }
}

void ScoreManager::RecordKill(uint32_t teamId) {
    EnsureTeamExists(teamId);
    m_scores[teamId].kills++;
    m_scores[teamId].score += 1;  // 1 point per kill
    Logger::Debug("Team %u kill recorded (total kills=%u)", teamId, m_scores[teamId].kills);
    BroadcastScores();
}

void ScoreManager::RecordDeath(uint32_t teamId) {
    EnsureTeamExists(teamId);
    m_scores[teamId].deaths++;
    Logger::Debug("Team %u death recorded (total deaths=%u)", teamId, m_scores[teamId].deaths);
    BroadcastScores();
}

void ScoreManager::RecordObjectiveCapture(uint32_t teamId, uint32_t points) {
    EnsureTeamExists(teamId);
    m_scores[teamId].objectivesCaptured++;
    m_scores[teamId].score += points;
    Logger::Info("Team %u captured objective (+%u points)", teamId, points);
    BroadcastScores();
}

void ScoreManager::AddPoints(uint32_t teamId, uint32_t points) {
    EnsureTeamExists(teamId);
    m_scores[teamId].score += points;
    Logger::Debug("Team %u awarded %u points (total=%u)", teamId, points, m_scores[teamId].score);
    BroadcastScores();
}

void ScoreManager::SetPoints(uint32_t teamId, uint32_t points) {
    EnsureTeamExists(teamId);
    m_scores[teamId].score = points;
    Logger::Debug("Team %u score set to %u", teamId, points);
    BroadcastScores();
}

TeamScore ScoreManager::GetTeamScore(uint32_t teamId) const {
    auto it = m_scores.find(teamId);
    if (it != m_scores.end()) {
        return it->second;
    }
    return TeamScore{};
}

std::vector<uint32_t> ScoreManager::GetTeamsByScore() const {
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto& [teamId, score] : m_scores) {
        entries.emplace_back(teamId, score.score);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto& a, auto& b){ return a.second > b.second; });
    std::vector<uint32_t> ordered;
    for (auto& e : entries) ordered.push_back(e.first);
    return ordered;
}

void ScoreManager::ResetAll() {
    m_scores.clear();
    if (m_teamManager) {
        for (uint32_t teamId = 1; teamId <= m_teamManager->GetTeamCount(); ++teamId) {
            m_scores[teamId] = TeamScore{};
        }
    }
    Logger::Info("ScoreManager: All scores reset");
    BroadcastScores();
}

void ScoreManager::BroadcastScores() const {
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
}