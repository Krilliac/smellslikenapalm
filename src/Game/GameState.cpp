// src/Game/GameState.cpp – Implementation for GameState

#include "Game/GameState.h"
#include "Game/GameServer.h"
#include "Game/GameMode.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Config/GameConfig.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include <algorithm>

GameState::GameState(GameServer* server)
    : m_server(server),
      m_currentPhase(GamePhase::Waiting),
      m_matchState(MatchState::NotStarted),
      m_currentRound(0),
      m_maxRounds(1),
      m_roundDuration(std::chrono::seconds(1800)), // 30 minutes default
      m_winningTeam(0),
      m_scoreLimit(100),
      m_objectiveLimit(5)
{
    Logger::Info("GameState initialized");
}

GameState::~GameState() = default;

void GameState::Initialize()
{
    Logger::Info("Initializing GameState...");
    
    // Load configuration
    auto gameConfig = m_server->GetGameConfig();
    if (gameConfig) {
        const auto& settings = gameConfig->GetGameSettings();
        m_roundDuration = std::chrono::seconds(settings.roundTimeLimit);
        m_maxRounds = 1; // Single round for most RS2V modes
        
        // Get game mode specific settings
        auto gameModedef = gameConfig->GetGameModeDefinition(settings.gameMode);
        if (gameModedef) {
            m_scoreLimit = gameModedef->scoreLimit;
            m_objectiveLimit = gameModedef->objectiveLimit;
        }
    }
    
    InitializeTeamScores();
    InitializeObjectives();
    
    Logger::Info("GameState initialized - Round duration: %ld seconds, Score limit: %u",
                m_roundDuration.count(), m_scoreLimit);
}

void GameState::Update()
{
    UpdateTimers();
    CheckPhaseTransition();
    
    // Check win conditions periodically
    if (m_currentPhase == GamePhase::Active) {
        if (CheckWinCondition()) {
            EndRound();
        }
    }
}

void GameState::Reset()
{
    Logger::Info("Resetting GameState");
    
    m_currentPhase = GamePhase::Waiting;
    m_matchState = MatchState::NotStarted;
    m_currentRound = 0;
    m_winningTeam = 0;
    m_winReason.clear();
    
    // Reset scores
    for (auto& score : m_teamScores) {
        score.score = 0;
        score.kills = 0;
        score.deaths = 0;
        score.objectivesCaptured = 0;
    }
    
    // Reset objectives
    for (auto& obj : m_objectives) {
        obj.controllingTeam = 0;
        obj.captureProgress = 0.0f;
        obj.isNeutral = true;
    }
    
    BroadcastGameState();
}

void GameState::SetPhase(GamePhase phase)
{
    if (m_currentPhase != phase) {
        GamePhase oldPhase = m_currentPhase;
        m_currentPhase = phase;
        m_phaseStartTime = std::chrono::steady_clock::now();
        
        LogStateChange("Phase changed from " + std::to_string(static_cast<int>(oldPhase)) + 
                      " to " + std::to_string(static_cast<int>(phase)));
        
        BroadcastPhaseChange();
    }
}

GamePhase GameState::GetPhase() const
{
    return m_currentPhase;
}

void GameState::AdvancePhase()
{
    switch (m_currentPhase) {
        case GamePhase::Waiting:
            if (m_server->GetTeamManager()->HasEnoughPlayers()) {
                SetPhase(GamePhase::Preparation);
            }
            break;
            
        case GamePhase::Preparation:
            StartRound();
            break;
            
        case GamePhase::Active:
            EndRound();
            break;
            
        case GamePhase::PostRound:
            if (m_currentRound >= m_maxRounds) {
                EndMatch();
                SetPhase(GamePhase::MapChanging);
            } else {
                SetPhase(GamePhase::Preparation);
            }
            break;
            
        case GamePhase::MapChanging:
            SetPhase(GamePhase::Waiting);
            break;
    }
}

void GameState::StartMatch()
{
    Logger::Info("Starting match");
    m_matchState = MatchState::InProgress;
    m_currentRound = 0;
    SetPhase(GamePhase::Preparation);
}

void GameState::EndMatch()
{
    Logger::Info("Ending match - Winner: Team %u", m_winningTeam);
    m_matchState = MatchState::Finished;
    
    // Broadcast final results
    BroadcastGameState();
    
    // Trigger map change after delay
    // This would typically be handled by the GameServer
}

void GameState::PauseMatch()
{
    if (m_matchState == MatchState::InProgress) {
        m_matchState = MatchState::Paused;
        Logger::Info("Match paused");
        BroadcastGameState();
    }
}

void GameState::ResumeMatch()
{
    if (m_matchState == MatchState::Paused) {
        m_matchState = MatchState::InProgress;
        Logger::Info("Match resumed");
        BroadcastGameState();
    }
}

MatchState GameState::GetMatchState() const
{
    return m_matchState;
}

void GameState::StartRound()
{
    m_currentRound++;
    m_roundStartTime = std::chrono::steady_clock::now();
    m_roundEndTime = m_roundStartTime + m_roundDuration;
    
    SetPhase(GamePhase::Active);
    
    Logger::Info("Round %u started (duration: %ld seconds)", 
                m_currentRound, m_roundDuration.count());
    
    // Spawn all players
    auto playerManager = m_server->GetPlayerManager();
    if (playerManager) {
        playerManager->SpawnAllPlayers();
    }
    
    BroadcastGameState();
}

void GameState::EndRound()
{
    Logger::Info("Round %u ended", m_currentRound);
    
    SetPhase(GamePhase::PostRound);
    
    // Determine round winner if not already set
    if (m_winningTeam == 0) {
        CheckWinCondition();
    }
    
    BroadcastScoreUpdate();
}

uint32_t GameState::GetCurrentRound() const
{
    return m_currentRound;
}

uint32_t GameState::GetMaxRounds() const
{
    return m_maxRounds;
}

std::chrono::steady_clock::time_point GameState::GetRoundStartTime() const
{
    return m_roundStartTime;
}

std::chrono::steady_clock::time_point GameState::GetRoundEndTime() const
{
    return m_roundEndTime;
}

std::chrono::seconds GameState::GetRemainingTime() const
{
    if (m_currentPhase != GamePhase::Active) {
        return std::chrono::seconds(0);
    }
    
    auto now = std::chrono::steady_clock::now();
    if (now >= m_roundEndTime) {
        return std::chrono::seconds(0);
    }
    
    return std::chrono::duration_cast<std::chrono::seconds>(m_roundEndTime - now);
}

bool GameState::IsTimeExpired() const
{
    return GetRemainingTime() <= std::chrono::seconds(0);
}

void GameState::AddObjective(uint32_t objectiveId, uint32_t initialTeam)
{
    ObjectiveState obj;
    obj.objectiveId = objectiveId;
    obj.controllingTeam = initialTeam;
    obj.captureProgress = 0.0f;
    obj.isNeutral = (initialTeam == 0);
    obj.lastCaptureTime = std::chrono::steady_clock::now();
    
    m_objectives.push_back(obj);
    
    Logger::Debug("Added objective %u (initial team: %u)", objectiveId, initialTeam);
}

void GameState::UpdateObjective(uint32_t objectiveId, uint32_t teamId, float progress)
{
    auto* obj = GetObjective(objectiveId);
    if (!obj) return;
    
    obj->captureProgress = std::clamp(progress, 0.0f, 1.0f);
    
    // Check if capture is complete
    if (obj->captureProgress >= 1.0f && obj->controllingTeam != teamId) {
        CaptureObjective(objectiveId, teamId);
    }
    
    BroadcastObjectiveUpdate(objectiveId);
}

void GameState::CaptureObjective(uint32_t objectiveId, uint32_t teamId)
{
    auto* obj = GetObjective(objectiveId);
    if (!obj) return;
    
    uint32_t previousTeam = obj->controllingTeam;
    obj->controllingTeam = teamId;
    obj->captureProgress = 1.0f;
    obj->isNeutral = false;
    obj->lastCaptureTime = std::chrono::steady_clock::now();
    
    // Award points
    AddTeamScore(teamId, 10); // 10 points per objective
    
    // Update team objective counter
    auto* teamScore = GetTeamScoreRef(teamId);
    if (teamScore) {
        teamScore->objectivesCaptured++;
    }
    
    Logger::Info("Objective %u captured by team %u (previous: %u)", 
                objectiveId, teamId, previousTeam);
    
    BroadcastObjectiveUpdate(objectiveId);
    BroadcastScoreUpdate();
}

const std::vector<ObjectiveState>& GameState::GetObjectives() const
{
    return m_objectives;
}

ObjectiveState* GameState::GetObjective(uint32_t objectiveId)
{
    auto it = std::find_if(m_objectives.begin(), m_objectives.end(),
                          [objectiveId](const ObjectiveState& obj) {
                              return obj.objectiveId == objectiveId;
                          });
    
    return it != m_objectives.end() ? &(*it) : nullptr;
}

void GameState::AddTeamScore(uint32_t teamId, uint32_t points)
{
    auto* teamScore = GetTeamScoreRef(teamId);
    if (teamScore) {
        teamScore->score += points;
        Logger::Debug("Team %u score: %u (+%u)", teamId, teamScore->score, points);
        BroadcastScoreUpdate();
    }
}

void GameState::SetTeamScore(uint32_t teamId, uint32_t score)
{
    auto* teamScore = GetTeamScoreRef(teamId);
    if (teamScore) {
        teamScore->score = score;
        Logger::Debug("Team %u score set to: %u", teamId, score);
        BroadcastScoreUpdate();
    }
}

uint32_t GameState::GetTeamScore(uint32_t teamId) const
{
    auto it = std::find_if(m_teamScores.begin(), m_teamScores.end(),
                          [teamId](const TeamScore& score) {
                              return score.teamId == teamId;
                          });
    
    return it != m_teamScores.end() ? it->score : 0;
}

void GameState::AddTeamKill(uint32_t teamId)
{
    auto* teamScore = GetTeamScoreRef(teamId);
    if (teamScore) {
        teamScore->kills++;
        // Award points for kills
        AddTeamScore(teamId, 1);
    }
}

void GameState::AddTeamDeath(uint32_t teamId)
{
    auto* teamScore = GetTeamScoreRef(teamId);
    if (teamScore) {
        teamScore->deaths++;
    }
}

const std::vector<TeamScore>& GameState::GetTeamScores() const
{
    return m_teamScores;
}

bool GameState::CheckWinCondition()
{
    if (m_currentPhase != GamePhase::Active) {
        return false;
    }
    
    // Check time limit
    if (IsTimeExpired()) {
        // Determine winner by score
        uint32_t highestScore = 0;
        uint32_t winningTeam = 0;
        
        for (const auto& score : m_teamScores) {
            if (score.score > highestScore) {
                highestScore = score.score;
                winningTeam = score.teamId;
            }
        }
        
        m_winningTeam = winningTeam;
        m_winReason = "Time limit reached";
        Logger::Info("Win condition: Time expired - Team %u wins with %u points", 
                    winningTeam, highestScore);
        return true;
    }
    
    // Check score limit
    for (const auto& score : m_teamScores) {
        if (score.score >= m_scoreLimit) {
            m_winningTeam = score.teamId;
            m_winReason = "Score limit reached";
            Logger::Info("Win condition: Score limit - Team %u wins with %u points", 
                        score.teamId, score.score);
            return true;
        }
    }
    
    // Check objective limit
    for (const auto& score : m_teamScores) {
        if (score.objectivesCaptured >= m_objectiveLimit) {
            m_winningTeam = score.teamId;
            m_winReason = "Objective limit reached";
            Logger::Info("Win condition: Objective limit - Team %u wins with %u objectives", 
                        score.teamId, score.objectivesCaptured);
            return true;
        }
    }
    
    // Check if all objectives captured by one team
    if (!m_objectives.empty()) {
        uint32_t firstTeam = m_objectives[0].controllingTeam;
        if (firstTeam != 0) {
            bool allSameTeam = std::all_of(m_objectives.begin(), m_objectives.end(),
                                          [firstTeam](const ObjectiveState& obj) {
                                              return obj.controllingTeam == firstTeam;
                                          });
            
            if (allSameTeam) {
                m_winningTeam = firstTeam;
                m_winReason = "All objectives captured";
                Logger::Info("Win condition: All objectives - Team %u wins", firstTeam);
                return true;
            }
        }
    }
    
    return false;
}

uint32_t GameState::GetWinningTeam() const
{
    return m_winningTeam;
}

std::string GameState::GetWinReason() const
{
    return m_winReason;
}

void GameState::BroadcastGameState()
{
    auto serialized = SerializeGameState();
    auto networkManager = m_server->GetNetworkManager();
    if (networkManager) {
        networkManager->BroadcastPacket("GAME_STATE", serialized);
    }
}

void GameState::BroadcastPhaseChange()
{
    std::string message;
    switch (m_currentPhase) {
        case GamePhase::Waiting:
            message = "Waiting for players...";
            break;
        case GamePhase::Preparation:
            message = "Round starting soon!";
            break;
        case GamePhase::Active:
            message = "Round " + std::to_string(m_currentRound) + " active!";
            break;
        case GamePhase::PostRound:
            message = "Round ended!";
            if (m_winningTeam > 0) {
                message += " Team " + std::to_string(m_winningTeam) + " wins!";
            }
            break;
        case GamePhase::MapChanging:
            message = "Changing map...";
            break;
    }
    
    m_server->BroadcastChatMessage("[GAME] " + message);
    BroadcastGameState();
}

void GameState::BroadcastScoreUpdate()
{
    BroadcastGameState();
}

void GameState::BroadcastObjectiveUpdate(uint32_t objectiveId)
{
    auto* obj = GetObjective(objectiveId);
    if (!obj) return;
    
    std::string message = "Objective " + std::to_string(objectiveId);
    if (obj->isNeutral) {
        message += " is contested!";
    } else {
        message += " captured by Team " + std::to_string(obj->controllingTeam) + "!";
    }
    
    m_server->BroadcastChatMessage("[OBJECTIVE] " + message);
    BroadcastGameState();
}

std::vector<uint8_t> GameState::SerializeGameState() const
{
    std::vector<uint8_t> data;
    
    // Add phase
    data.push_back(static_cast<uint8_t>(m_currentPhase));
    data.push_back(static_cast<uint8_t>(m_matchState));
    
    // Add round info
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&m_currentRound),
                reinterpret_cast<const uint8_t*>(&m_currentRound) + sizeof(m_currentRound));
    
    // Add remaining time
    auto remaining = GetRemainingTime().count();
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&remaining),
                reinterpret_cast<const uint8_t*>(&remaining) + sizeof(remaining));
    
    // Add team scores
    uint32_t teamCount = static_cast<uint32_t>(m_teamScores.size());
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&teamCount),
                reinterpret_cast<const uint8_t*>(&teamCount) + sizeof(teamCount));
    
    for (const auto& score : m_teamScores) {
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&score),
                    reinterpret_cast<const uint8_t*>(&score) + sizeof(score));
    }
    
    // Add objectives
    uint32_t objCount = static_cast<uint32_t>(m_objectives.size());
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&objCount),
                reinterpret_cast<const uint8_t*>(&objCount) + sizeof(objCount));
    
    for (const auto& obj : m_objectives) {
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&obj),
                    reinterpret_cast<const uint8_t*>(&obj) + sizeof(obj));
    }
    
    return data;
}

void GameState::DeserializeGameState(const std::vector<uint8_t>& data)
{
    if (data.empty()) return;
    
    size_t offset = 0;
    
    // Read phase
    if (offset < data.size()) {
        m_currentPhase = static_cast<GamePhase>(data[offset++]);
    }
    if (offset < data.size()) {
        m_matchState = static_cast<MatchState>(data[offset++]);
    }
    
    // Read round info
    if (offset + sizeof(m_currentRound) <= data.size()) {
        std::memcpy(&m_currentRound, &data[offset], sizeof(m_currentRound));
        offset += sizeof(m_currentRound);
    }
    
    // Skip remaining time (read-only on client)
    offset += sizeof(int64_t);
    
    // Read team scores
    uint32_t teamCount = 0;
    if (offset + sizeof(teamCount) <= data.size()) {
        std::memcpy(&teamCount, &data[offset], sizeof(teamCount));
        offset += sizeof(teamCount);
        
        m_teamScores.clear();
        for (uint32_t i = 0; i < teamCount && offset + sizeof(TeamScore) <= data.size(); ++i) {
            TeamScore score;
            std::memcpy(&score, &data[offset], sizeof(score));
            m_teamScores.push_back(score);
            offset += sizeof(score);
        }
    }
    
    // Read objectives
    uint32_t objCount = 0;
    if (offset + sizeof(objCount) <= data.size()) {
        std::memcpy(&objCount, &data[offset], sizeof(objCount));
        offset += sizeof(objCount);
        
        m_objectives.clear();
        for (uint32_t i = 0; i < objCount && offset + sizeof(ObjectiveState) <= data.size(); ++i) {
            ObjectiveState obj;
            std::memcpy(&obj, &data[offset], sizeof(obj));
            m_objectives.push_back(obj);
            offset += sizeof(obj);
        }
    }
}

// Private helper methods
void GameState::InitializeTeamScores()
{
    m_teamScores.clear();
    
    auto teamManager = m_server->GetTeamManager();
    if (teamManager) {
        for (uint32_t teamId = 1; teamId <= teamManager->GetTeamCount(); ++teamId) {
            TeamScore score;
            score.teamId = teamId;
            score.score = 0;
            score.kills = 0;
            score.deaths = 0;
            score.objectivesCaptured = 0;
            m_teamScores.push_back(score);
        }
    }
    
    Logger::Debug("Initialized %zu team scores", m_teamScores.size());
}

void GameState::InitializeObjectives()
{
    m_objectives.clear();
    
    auto mapManager = m_server->GetMapManager();
    if (mapManager) {
        auto objectives = mapManager->GetMapObjectives();
        for (uint32_t objId : objectives) {
            AddObjective(objId, 0); // Start neutral
        }
    }
    
    Logger::Debug("Initialized %zu objectives", m_objectives.size());
}

void GameState::UpdateTimers()
{
    // Timer updates are handled in getter methods
    // This could be extended for more complex timing logic
}

void GameState::CheckPhaseTransition()
{
    auto now = std::chrono::steady_clock::now();
    auto phaseDuration = now - m_phaseStartTime;
    
    switch (m_currentPhase) {
        case GamePhase::Preparation:
            // Auto-advance after preparation time
            if (phaseDuration >= std::chrono::seconds(30)) { // 30 second prep
                AdvancePhase();
            }
            break;
            
        case GamePhase::Active:
            // Check for time expiry
            if (IsTimeExpired()) {
                AdvancePhase();
            }
            break;
            
        case GamePhase::PostRound:
            // Auto-advance after showing results
            if (phaseDuration >= std::chrono::seconds(15)) { // 15 second results
                AdvancePhase();
            }
            break;
            
        default:
            break;
    }
}

TeamScore* GameState::GetTeamScoreRef(uint32_t teamId)
{
    auto it = std::find_if(m_teamScores.begin(), m_teamScores.end(),
                          [teamId](const TeamScore& score) {
                              return score.teamId == teamId;
                          });
    
    return it != m_teamScores.end() ? &(*it) : nullptr;
}

void GameState::LogStateChange(const std::string& change)
{
    Logger::Info("GameState: %s", change.c_str());
}