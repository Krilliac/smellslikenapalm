// src/Game/GameMode.cpp – Complete implementation for RS2V Server GameMode

#include "Game/GameMode.h"
#include "Utils/Logger.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/MapManager.h"
#include "Game/TeamManager.h"
#include <algorithm>
#include <cstring>

GameMode::GameMode(GameServer* server, const GameModeDefinition& def)
    : m_server(server), m_def(def)
{
    Logger::Trace("[GameMode::GameMode] Entry, name='%s'", def.name.c_str());
    Logger::Info("GameMode '%s' initialized", m_def.name.c_str());
    Logger::Trace("[GameMode::GameMode] Exit");
}

GameMode::~GameMode()
{
    Logger::Trace("[GameMode::~GameMode] Entry");
    Logger::Trace("[GameMode::~GameMode] Exit");
}

void GameMode::OnStart()
{
    Logger::Trace("[GameMode::OnStart] Entry");
    Logger::Info("GameMode '%s' starting", m_def.name.c_str());
    m_tickCount = 0;
    m_phase = Phase::Preparation;
    m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Logger::Debug("[GameMode::OnStart] Phase set to Preparation, tickCount reset to 0, phaseEndTime set to 30s from now");
    BroadcastPhase();
    Logger::Trace("[GameMode::OnStart] Exit");
}

void GameMode::OnEnd()
{
    Logger::Trace("[GameMode::OnEnd] Entry");
    Logger::Info("GameMode '%s' ending", m_def.name.c_str());
    // Clean up, tally scores, notify players
    m_server->BroadcastChatMessage("[GameMode] Round results tallied.");
    Logger::Debug("[GameMode::OnEnd] Round results broadcast complete");
    Logger::Trace("[GameMode::OnEnd] Exit");
}

void GameMode::Update()
{
    Logger::Trace("[GameMode::Update] Entry, tickCount=%lu", (unsigned long)m_tickCount);
    auto now = std::chrono::steady_clock::now();
    if (now >= m_phaseEndTime) {
        Logger::Debug("[GameMode::Update] Phase end time reached, advancing phase");
        AdvancePhase();
    }
    m_tickCount++;
    // Periodic tasks
    if (m_tickCount % 60 == 0) {
        Logger::Debug("[GameMode::Update] Tick %lu: Running periodic respawn check", (unsigned long)m_tickCount);
        HandleRespawns();
    }
    Logger::Trace("[GameMode::Update] Exit");
}

void GameMode::HandlePlayerAction(uint32_t playerId, const std::string& action, const std::vector<uint8_t>& data)
{
    Logger::Trace("[GameMode::HandlePlayerAction] Entry, playerId=%u, action='%s', dataSize=%zu",
                 playerId, action.c_str(), data.size());
    // GameMode-specific action handling
    if (m_phase != Phase::Active) {
        Logger::Debug("Ignoring action '%s' by %u outside active phase", action.c_str(), playerId);
        Logger::Trace("[GameMode::HandlePlayerAction] Exit (not active phase)");
        return;
    }
    // e.g., objective captures
    if (action == "capture_objective") {
        uint32_t objId = 0;
        if (data.size() >= 4) {
            memcpy(&objId, data.data(), sizeof(uint32_t));
        }
        Logger::Debug("[GameMode::HandlePlayerAction] Player %u capturing objective %u", playerId, objId);
        HandleObjectiveCapture(playerId, objId);
    } else {
        Logger::Debug("[GameMode::HandlePlayerAction] Unhandled action '%s' from player %u", action.c_str(), playerId);
    }
    Logger::Trace("[GameMode::HandlePlayerAction] Exit");
}

void GameMode::AdvancePhase()
{
    Logger::Trace("[GameMode::AdvancePhase] Entry, currentPhase=%d", static_cast<int>(m_phase));
    switch (m_phase) {
        case Phase::Preparation:
            m_phase = Phase::Active;
            m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(m_def.roundTimeLimit);
            Logger::Info("GameMode '%s' entering Active phase", m_def.name.c_str());
            Logger::Debug("[GameMode::AdvancePhase] Transitioned Preparation -> Active, roundTimeLimit=%d", m_def.roundTimeLimit);
            break;
        case Phase::Active:
            m_phase = Phase::PostRound;
            m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            Logger::Info("GameMode '%s' entering PostRound phase", m_def.name.c_str());
            Logger::Debug("[GameMode::AdvancePhase] Transitioned Active -> PostRound");
            break;
        case Phase::PostRound:
            Logger::Debug("[GameMode::AdvancePhase] PostRound complete, ending GameMode and triggering map change");
            OnEnd();
            m_server->ChangeMap();  // trigger map rotation
            Logger::Trace("[GameMode::AdvancePhase] Exit (PostRound -> map change)");
            return;
    }
    BroadcastPhase();
    Logger::Trace("[GameMode::AdvancePhase] Exit");
}

void GameMode::BroadcastPhase() const
{
    Logger::Trace("[GameMode::BroadcastPhase] Entry, phase=%d", static_cast<int>(m_phase));
    std::string msg;
    switch (m_phase) {
        case Phase::Preparation: msg = "Round starting soon!"; break;
        case Phase::Active:      msg = "Round is now active!";  break;
        case Phase::PostRound:   msg = "Round over!";           break;
    }
    Logger::Debug("[GameMode::BroadcastPhase] Broadcasting phase message: '%s'", msg.c_str());
    m_server->BroadcastChatMessage("[GameMode] " + msg);
    Logger::Trace("[GameMode::BroadcastPhase] Exit");
}

void GameMode::HandleRespawns()
{
    Logger::Trace("[GameMode::HandleRespawns] Entry");
    auto* pm = m_server->GetPlayerManager();
    auto deadPlayers = pm->GetDeadPlayers();
    Logger::Debug("[GameMode::HandleRespawns] Found %zu dead players to respawn", deadPlayers.size());
    for (auto& p : deadPlayers) {
        uint32_t id = p->GetConnection()->GetClientId();
        Logger::Debug("[GameMode::HandleRespawns] Respawning player %u", id);
        pm->OnPlayerSpawn(id);
    }
    Logger::Trace("[GameMode::HandleRespawns] Exit");
}

void GameMode::HandleObjectiveCapture(uint32_t playerId, uint32_t objectiveId)
{
    Logger::Trace("[GameMode::HandleObjectiveCapture] Entry, playerId=%u, objectiveId=%u", playerId, objectiveId);
    auto* tm = m_server->GetTeamManager();
    uint32_t team = tm->GetPlayerTeam(playerId);
    Logger::Debug("[GameMode::HandleObjectiveCapture] Player %u is on team %u", playerId, team);
    if (tm->CaptureObjective(team, objectiveId)) {
        Logger::Debug("[GameMode::HandleObjectiveCapture] Objective %u captured by team %u, broadcasting", objectiveId, team);
        m_server->BroadcastChatMessage(
            "Player " + std::to_string(playerId) +
            " captured objective " + std::to_string(objectiveId)
        );

        // Award points
        tm->AddTeamScore(team, 100);
        Logger::Debug("[GameMode::HandleObjectiveCapture] Awarded 100 points to team %u", team);

        // Check win condition
        if (tm->GetTeamScore(team) >= m_def.scoreLimit) {
            Logger::Info("Team %u reached score limit, ending game", team);
            m_phase = Phase::PostRound;
            m_phaseEndTime = std::chrono::steady_clock::now();
        } else {
            Logger::Debug("[GameMode::HandleObjectiveCapture] Team %u score=%u, scoreLimit=%u, game continues",
                         team, tm->GetTeamScore(team), m_def.scoreLimit);
        }
    } else {
        Logger::Debug("[GameMode::HandleObjectiveCapture] Capture of objective %u by team %u failed", objectiveId, team);
    }
    Logger::Trace("[GameMode::HandleObjectiveCapture] Exit");
}

// Accessors
const std::string& GameMode::GetName() const {
    Logger::Trace("[GameMode::GetName] Entry/Exit, returning '%s'", m_def.name.c_str());
    return m_def.name;
}
GameMode::Phase GameMode::GetPhase() const {
    Logger::Trace("[GameMode::GetPhase] Entry/Exit, returning %d", static_cast<int>(m_phase));
    return m_phase;
}
uint64_t GameMode::GetTickCount() const {
    Logger::Trace("[GameMode::GetTickCount] Entry/Exit, returning %lu", (unsigned long)m_tickCount);
    return m_tickCount;
}
