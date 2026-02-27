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
    Logger::Info("GameMode '%s' initialized", m_def.name.c_str());
}

GameMode::~GameMode() = default;

void GameMode::OnStart()
{
    Logger::Info("GameMode '%s' starting", m_def.name.c_str());
    m_tickCount = 0;
    m_phase = Phase::Preparation;
    m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    BroadcastPhase();
}

void GameMode::OnEnd()
{
    Logger::Info("GameMode '%s' ending", m_def.name.c_str());
    // Clean up, tally scores, notify players
    m_server->BroadcastChatMessage("[GameMode] Round results tallied.");
}

void GameMode::Update()
{
    auto now = std::chrono::steady_clock::now();
    if (now >= m_phaseEndTime) {
        AdvancePhase();
    }
    m_tickCount++;
    // Periodic tasks
    if (m_tickCount % 60 == 0) {
        HandleRespawns();
    }
}

void GameMode::HandlePlayerAction(uint32_t playerId, const std::string& action, const std::vector<uint8_t>& data)
{
    // GameMode-specific action handling
    if (m_phase != Phase::Active) {
        Logger::Debug("Ignoring action '%s' by %u outside active phase", action.c_str(), playerId);
        return;
    }
    // e.g., objective captures
    if (action == "capture_objective") {
        uint32_t objId = 0;
        if (data.size() >= 4) {
            memcpy(&objId, data.data(), sizeof(uint32_t));
        }
        HandleObjectiveCapture(playerId, objId);
    }
}

void GameMode::AdvancePhase()
{
    switch (m_phase) {
        case Phase::Preparation:
            m_phase = Phase::Active;
            m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(m_def.roundTimeLimit);
            Logger::Info("GameMode '%s' entering Active phase", m_def.name.c_str());
            break;
        case Phase::Active:
            m_phase = Phase::PostRound;
            m_phaseEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            Logger::Info("GameMode '%s' entering PostRound phase", m_def.name.c_str());
            break;
        case Phase::PostRound:
            OnEnd();
            m_server->ChangeMap();  // trigger map rotation
            return;
    }
    BroadcastPhase();
}

void GameMode::BroadcastPhase() const
{
    std::string msg;
    switch (m_phase) {
        case Phase::Preparation: msg = "Round starting soon!"; break;
        case Phase::Active:      msg = "Round is now active!";  break;
        case Phase::PostRound:   msg = "Round over!";           break;
    }
    m_server->BroadcastChatMessage("[GameMode] " + msg);
}

void GameMode::HandleRespawns()
{
    auto* pm = m_server->GetPlayerManager();
    for (auto& p : pm->GetDeadPlayers()) {
        uint32_t id = p->GetConnection()->GetClientId();
        pm->OnPlayerSpawn(id);
    }
}

void GameMode::HandleObjectiveCapture(uint32_t playerId, uint32_t objectiveId)
{
    auto* tm = m_server->GetTeamManager();
    uint32_t team = tm->GetPlayerTeam(playerId);
    if (tm->CaptureObjective(team, objectiveId)) {
        m_server->BroadcastChatMessage(
            "Player " + std::to_string(playerId) +
            " captured objective " + std::to_string(objectiveId)
        );

        // Award points
        tm->AddTeamScore(team, 100);

        // Check win condition
        if (tm->GetTeamScore(team) >= m_def.scoreLimit) {
            Logger::Info("Team %u reached score limit, ending game", team);
            m_phase = Phase::PostRound;
            m_phaseEndTime = std::chrono::steady_clock::now();
        }
    }
}

// Accessors
const std::string& GameMode::GetName() const { return m_def.name; }
GameMode::Phase GameMode::GetPhase() const { return m_phase; }
uint64_t GameMode::GetTickCount() const { return m_tickCount; }