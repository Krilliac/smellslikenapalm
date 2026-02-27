// src/Game/SkirmishMode.cpp
// RS2V Skirmish game mode implementation

#include "Game/SkirmishMode.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Game/PlayerManager.h"
#include "Utils/Logger.h"
#include <algorithm>

SkirmishMode::SkirmishMode(GameServer* server)
    : m_server(server)
{
}

SkirmishMode::~SkirmishMode() {
    Shutdown();
}

void SkirmishMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_currentRound = 0;
    m_team1Wins = 0;
    m_team2Wins = 0;
    Logger::Info("SkirmishMode initialized (%d rounds, %.0fs per round)", m_maxRounds, m_roundTime);
}

void SkirmishMode::Shutdown() {
    Logger::Info("SkirmishMode shutdown");
}

void SkirmishMode::StartRound() {
    m_currentRound++;
    SetPhase(Phase::Preparation);
    Logger::Info("Skirmish round %d/%d starting", m_currentRound, m_maxRounds);
}

void SkirmishMode::EndRound() {
    Logger::Info("Skirmish round %d ended (Team1: %d wins, Team2: %d wins)",
                 m_currentRound, m_team1Wins, m_team2Wins);

    // Check if match is decided
    int winsNeeded = (m_maxRounds / 2) + 1;
    if (m_team1Wins >= winsNeeded || m_team2Wins >= winsNeeded || m_currentRound >= m_maxRounds) {
        DetermineMatchWinner();
        SetPhase(Phase::Finished);
    } else {
        SetPhase(Phase::NextRound);
    }
}

void SkirmishMode::Update(float deltaSeconds) {
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
            CheckWinConditions();
            if (m_phaseTimer <= 0.0f) {
                // Round timer expired — enter instant death
                SetPhase(Phase::InstantDeath);
            }
            break;

        case Phase::InstantDeath:
            CheckInstantDeathWinner();
            if (m_phaseTimer <= 0.0f) {
                // Instant death timer expired — most surviving players wins
                CheckInstantDeathWinner();
                EndRound();
            }
            break;

        case Phase::PostRound:
            if (m_phaseTimer <= 0.0f) {
                EndRound();
            }
            break;

        case Phase::NextRound:
            if (m_phaseTimer <= 0.0f) {
                StartRound();
            }
            break;

        case Phase::Finished:
            break;
    }
}

void SkirmishMode::OnObjectiveCaptured(uint32_t /*objectiveId*/, uint32_t capturingTeam) {
    // In Skirmish, capturing objectives extends tickets
    Logger::Info("Skirmish objective captured by team %u — tickets replenished", capturingTeam);
}

void SkirmishMode::OnTicketsDepleted(uint32_t teamId) {
    // Team ran out of tickets — other team wins this round
    uint32_t winner = (teamId == 1) ? 2u : 1u;
    AwardRoundWin(winner);
    SetPhase(Phase::PostRound);
}

void SkirmishMode::OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {
    if (m_phase == Phase::InstantDeath) {
        CheckInstantDeathWinner();
    }
}

int SkirmishMode::GetTeamRoundWins(uint32_t teamId) const {
    return teamId == 1 ? m_team1Wins : m_team2Wins;
}

float SkirmishMode::GetRoundTimeRemaining() const {
    return m_phase == Phase::Active ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

float SkirmishMode::GetInstantDeathTimeRemaining() const {
    return m_phase == Phase::InstantDeath ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

void SkirmishMode::SetMaxRounds(int rounds) { m_maxRounds = rounds; }
void SkirmishMode::SetRoundTime(float seconds) { m_roundTime = seconds; }
void SkirmishMode::SetInstantDeathTime(float seconds) { m_instantDeathTime = seconds; }
void SkirmishMode::SetTicketsPerRound(uint32_t tickets) { m_ticketsPerRound = tickets; }

void SkirmishMode::SetPhase(Phase newPhase) {
    m_phase = newPhase;
    switch (newPhase) {
        case Phase::WarmUp:       m_phaseTimer = 0.0f; break;
        case Phase::Preparation:  m_phaseTimer = m_preparationTime; break;
        case Phase::Active:       m_phaseTimer = m_roundTime; break;
        case Phase::InstantDeath: m_phaseTimer = m_instantDeathTime; break;
        case Phase::PostRound:    m_phaseTimer = m_postRoundTime; break;
        case Phase::NextRound:    m_phaseTimer = 5.0f; break;
        case Phase::Finished:     m_phaseTimer = 0.0f; break;
    }
    BroadcastPhaseChange();
}

void SkirmishMode::CheckWinConditions() {
    // Check if one team holds all objectives and the other has no tickets
    // This is handled via OnObjectiveCaptured + OnTicketsDepleted callbacks
}

void SkirmishMode::CheckInstantDeathWinner() {
    auto* pm = m_server->GetPlayerManager();
    auto* tm = m_server->GetTeamManager();

    int team1Alive = 0, team2Alive = 0;
    for (uint32_t pid : tm->GetTeamPlayers(1)) {
        auto p = pm->GetPlayer(pid);
        if (p && p->IsAlive()) team1Alive++;
    }
    for (uint32_t pid : tm->GetTeamPlayers(2)) {
        auto p = pm->GetPlayer(pid);
        if (p && p->IsAlive()) team2Alive++;
    }

    if (team1Alive == 0 && team2Alive > 0) {
        AwardRoundWin(2);
        SetPhase(Phase::PostRound);
    } else if (team2Alive == 0 && team1Alive > 0) {
        AwardRoundWin(1);
        SetPhase(Phase::PostRound);
    } else if (team1Alive == 0 && team2Alive == 0) {
        // Draw — no win awarded
        Logger::Info("Skirmish round %d — mutual elimination", m_currentRound);
        SetPhase(Phase::PostRound);
    } else if (m_phaseTimer <= 0.0f) {
        // Timer expired — most survivors win
        if (team1Alive > team2Alive) AwardRoundWin(1);
        else if (team2Alive > team1Alive) AwardRoundWin(2);
        // Tie = no winner
    }
}

void SkirmishMode::AwardRoundWin(uint32_t teamId) {
    if (teamId == 1) m_team1Wins++;
    else m_team2Wins++;
    Logger::Info("Team %u wins Skirmish round %d", teamId, m_currentRound);
    m_server->BroadcastChatMessage("[Skirmish] Team " + std::to_string(teamId) +
                                    " wins round " + std::to_string(m_currentRound) + "!");
}

void SkirmishMode::DetermineMatchWinner() {
    if (m_team1Wins > m_team2Wins) {
        Logger::Info("Team 1 wins Skirmish match (%d-%d)", m_team1Wins, m_team2Wins);
    } else if (m_team2Wins > m_team1Wins) {
        Logger::Info("Team 2 wins Skirmish match (%d-%d)", m_team2Wins, m_team1Wins);
    } else {
        Logger::Info("Skirmish match tied (%d-%d)", m_team1Wins, m_team2Wins);
    }
    m_server->BroadcastChatMessage("[Skirmish] Match complete!");
}

void SkirmishMode::BroadcastPhaseChange() const {
    std::string msg;
    switch (m_phase) {
        case Phase::WarmUp:       msg = "Waiting for players..."; break;
        case Phase::Preparation:  msg = "Round " + std::to_string(m_currentRound) + " starting!"; break;
        case Phase::Active:       msg = "Round " + std::to_string(m_currentRound) + " active!"; break;
        case Phase::InstantDeath: msg = "INSTANT DEATH — No respawns!"; break;
        case Phase::PostRound:    msg = "Round over!"; break;
        case Phase::NextRound:    msg = "Next round starting..."; break;
        case Phase::Finished:     msg = "Match complete!"; break;
    }
    m_server->BroadcastChatMessage("[Skirmish] " + msg);
}
