// src/Game/GameState.cpp – Implementation for GameState

#include "Game/GameState.h"
#include "Game/GameServer.h"
#include "Game/GameMode.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Config/GameConfig.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace {

template <typename T>
void AppendScalar(std::vector<uint8_t>& data, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* begin = reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(), begin, begin + sizeof(T));
}

bool CanRead(const std::vector<uint8_t>& data, size_t offset, size_t size)
{
    return offset <= data.size() && size <= data.size() - offset;
}

template <typename T>
bool ReadScalar(const std::vector<uint8_t>& data, size_t& offset, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!CanRead(data, offset, sizeof(T))) {
        return false;
    }

    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

// GAME_STATE predates the structured retail replication path and originally
// copied MSVC x64 structs directly. Freeze that legacy byte contract explicitly
// so future host ABI changes cannot silently alter the wire format.
constexpr size_t kTeamScoreWireSize = 20u;
constexpr size_t kObjectiveStateWireSize = 24u;
static_assert(sizeof(uint32_t) == 4u);
static_assert(sizeof(float) == 4u);
static_assert(sizeof(int64_t) == 8u);
static_assert(std::endian::native == std::endian::little);

} // namespace

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
            m_objectiveLimit = 5; // default objective limit
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
        for (auto& p : playerManager->GetDeadPlayers()) {
            uint32_t id = p->GetConnection()->GetClientId();
            playerManager->OnPlayerSpawn(id);
        }
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
    AppendScalar(data, static_cast<uint8_t>(m_currentPhase));
    AppendScalar(data, static_cast<uint8_t>(m_matchState));

    // Add round info
    AppendScalar(data, m_currentRound);

    // Add remaining time
    const int64_t remaining = static_cast<int64_t>(GetRemainingTime().count());
    AppendScalar(data, remaining);

    // Add team scores
    const uint32_t teamCount = static_cast<uint32_t>(m_teamScores.size());
    AppendScalar(data, teamCount);

    for (const auto& score : m_teamScores) {
        AppendScalar(data, score.teamId);
        AppendScalar(data, score.score);
        AppendScalar(data, score.kills);
        AppendScalar(data, score.deaths);
        AppendScalar(data, score.objectivesCaptured);
    }

    // Add objectives
    const uint32_t objCount = static_cast<uint32_t>(m_objectives.size());
    AppendScalar(data, objCount);

    for (const auto& obj : m_objectives) {
        AppendScalar(data, obj.objectiveId);
        AppendScalar(data, obj.controllingTeam);
        AppendScalar(data, obj.captureProgress);
        AppendScalar(data, static_cast<uint8_t>(obj.isNeutral));

        // The former raw-struct wire format placed the clock value at the next
        // eight-byte boundary. Keep those bytes for compatibility, but make
        // their value deterministic rather than copying struct padding.
        data.insert(data.end(), 3, uint8_t{0});

        const int64_t captureTicks = static_cast<int64_t>(
            obj.lastCaptureTime.time_since_epoch().count());
        AppendScalar(data, captureTicks);
    }

    return data;
}

void GameState::DeserializeGameState(const std::vector<uint8_t>& data)
{
    if (data.empty()) {
        return;
    }

    size_t offset = 0;

    // Decode into temporaries so a truncated packet cannot partially replace
    // live game state.
    uint8_t phaseValue = 0;
    uint8_t matchStateValue = 0;
    uint32_t currentRound = 0;
    uint32_t teamCount = 0;
    if (!ReadScalar(data, offset, phaseValue) ||
        !ReadScalar(data, offset, matchStateValue) ||
        !ReadScalar(data, offset, currentRound) ||
        !CanRead(data, offset, sizeof(int64_t))) {
        return;
    }

    offset += sizeof(int64_t); // Remaining time is read-only on this side.
    if (!ReadScalar(data, offset, teamCount)) {
        return;
    }

    if (teamCount > (data.size() - offset) / kTeamScoreWireSize) {
        return;
    }

    std::vector<TeamScore> teamScores;
    teamScores.reserve(teamCount);
    for (uint32_t i = 0; i < teamCount; ++i) {
        TeamScore score{};
        if (!ReadScalar(data, offset, score.teamId) ||
            !ReadScalar(data, offset, score.score) ||
            !ReadScalar(data, offset, score.kills) ||
            !ReadScalar(data, offset, score.deaths) ||
            !ReadScalar(data, offset, score.objectivesCaptured)) {
            return;
        }
        teamScores.push_back(score);
    }

    uint32_t objCount = 0;
    if (!ReadScalar(data, offset, objCount) ||
        objCount > (data.size() - offset) / kObjectiveStateWireSize) {
        return;
    }

    std::vector<ObjectiveState> objectives;
    objectives.reserve(objCount);
    for (uint32_t i = 0; i < objCount; ++i) {
        ObjectiveState obj{};
        uint8_t isNeutral = 0;
        int64_t captureTicks = 0;
        if (!ReadScalar(data, offset, obj.objectiveId) ||
            !ReadScalar(data, offset, obj.controllingTeam) ||
            !ReadScalar(data, offset, obj.captureProgress) ||
            !ReadScalar(data, offset, isNeutral) ||
            !CanRead(data, offset, 3)) {
            return;
        }

        offset += 3; // Reserved alignment bytes in the legacy wire layout.
        if (!ReadScalar(data, offset, captureTicks)) {
            return;
        }

        obj.isNeutral = isNeutral != 0;
        using ClockDuration = std::chrono::steady_clock::duration;
        obj.lastCaptureTime = std::chrono::steady_clock::time_point(
            ClockDuration(static_cast<ClockDuration::rep>(captureTicks)));
        objectives.push_back(obj);
    }

    m_currentPhase = static_cast<GamePhase>(phaseValue);
    m_matchState = static_cast<MatchState>(matchStateValue);
    m_currentRound = currentRound;
    m_teamScores = std::move(teamScores);
    m_objectives = std::move(objectives);
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
