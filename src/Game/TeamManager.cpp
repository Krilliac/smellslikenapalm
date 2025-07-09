// src/Game/TeamManager.cpp – Implementation for TeamManager

#include "Game/TeamManager.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Utils/Logger.h"

TeamManager::TeamManager(GameServer* server)
    : m_server(server)
{
    Logger::Info("TeamManager initialized");
}

TeamManager::~TeamManager()
{
    Shutdown();
}

void TeamManager::Initialize()
{
    Logger::Info("TeamManager: Setup default teams");
    // Example: two teams
    EnsureTeamExists(1, "US Army");
    EnsureTeamExists(2, "NVA");
}

void TeamManager::Shutdown()
{
    Logger::Info("TeamManager: Clearing teams");
    m_teams.clear();
    m_playerTeamMap.clear();
}

void TeamManager::EnsureTeamExists(uint32_t teamId, const std::string& name)
{
    if (m_teams.find(teamId) == m_teams.end()) {
        TeamInfo info;
        info.teamId = teamId;
        info.name = name.empty() ? ("Team " + std::to_string(teamId)) : name;
        m_teams[teamId] = info;
        Logger::Debug("TeamManager: Created team %u (%s)", teamId, info.name.c_str());
    }
}

void TeamManager::AddPlayerToTeam(uint32_t playerId, uint32_t teamId)
{
    auto pl = m_server->GetPlayerManager()->GetPlayer(playerId);
    if (!pl) return;
    RemovePlayer(playerId);
    EnsureTeamExists(teamId);
    m_playerTeamMap[playerId] = teamId;
    m_teams[teamId].playerIds.push_back(playerId);
    pl->SetTeam(teamId);
    Logger::Info("Player %u assigned to team %u", playerId, teamId);
}

void TeamManager::RemovePlayer(uint32_t playerId)
{
    auto it = m_playerTeamMap.find(playerId);
    if (it != m_playerTeamMap.end()) {
        uint32_t teamId = it->second;
        auto& vec = m_teams[teamId].playerIds;
        vec.erase(std::remove(vec.begin(), vec.end(), playerId), vec.end());
        m_playerTeamMap.erase(it);
        Logger::Debug("Player %u removed from team %u", playerId, teamId);
    }
}

uint32_t TeamManager::GetPlayerTeam(uint32_t playerId) const
{
    auto it = m_playerTeamMap.find(playerId);
    return it != m_playerTeamMap.end() ? it->second : 0;
}

std::vector<uint32_t> TeamManager::GetTeamPlayers(uint32_t teamId) const
{
    auto it = m_teams.find(teamId);
    return it != m_teams.end() ? it->second.playerIds : std::vector<uint32_t>();
}

std::vector<uint32_t> TeamManager::GetAllTeams() const
{
    std::vector<uint32_t> keys;
    for (auto& kv : m_teams) keys.push_back(kv.first);
    return keys;
}

size_t TeamManager::GetTeamCount() const
{
    return m_teams.size();
}

void TeamManager::AddTeamScore(uint32_t teamId, uint32_t points)
{
    EnsureTeamExists(teamId);
    m_teams[teamId].score += points;
    Logger::Debug("Team %u score increased by %u to %u", teamId, points, m_teams[teamId].score);
}

uint32_t TeamManager::GetTeamScore(uint32_t teamId) const
{
    auto it = m_teams.find(teamId);
    return it != m_teams.end() ? it->second.score : 0;
}

void TeamManager::ResetScores()
{
    for (auto& kv : m_teams) {
        kv.second.score = 0;
        kv.second.objectivesCaptured = 0;
    }
    Logger::Info("TeamManager: Scores reset");
}

bool TeamManager::CaptureObjective(uint32_t teamId, uint32_t objectiveId)
{
    EnsureTeamExists(teamId);
    m_teams[teamId].objectivesCaptured++;
    Logger::Info("Team %u has captured objective %u (total %u)", 
                 teamId, objectiveId, m_teams[teamId].objectivesCaptured);
    return true;
}

uint32_t TeamManager::GetObjectivesCaptured(uint32_t teamId) const
{
    auto it = m_teams.find(teamId);
    return it != m_teams.end() ? it->second.objectivesCaptured : 0;
}

bool TeamManager::HasEnoughPlayers() const
{
    // Require at least one player per team to start
    for (auto& kv : m_teams) {
        if (kv.second.playerIds.empty()) return false;
    }
    return true;
}

void TeamManager::AutoBalanceTeams()
{
    // Simple balance: move extra players from largest to smallest team
    if (m_teams.size() < 2) return;

    // Find team sizes
    std::vector<std::pair<uint32_t,size_t>> sizes;
    for (auto& kv : m_teams) {
        sizes.emplace_back(kv.first, kv.second.playerIds.size());
    }
    std::sort(sizes.begin(), sizes.end(),
              [](auto&a, auto&b){ return a.second > b.second; });
    auto [maxTeam, maxCount] = sizes.front();
    auto [minTeam, minCount] = sizes.back();

    if (maxCount > minCount + 1) {
        // Move one player
        uint32_t playerId = m_teams[maxTeam].playerIds.back();
        AddPlayerToTeam(playerId, minTeam);
        Logger::Info("AutoBalanced player %u from team %u to team %u",
                     playerId, maxTeam, minTeam);
    }
}