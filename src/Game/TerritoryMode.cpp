// src/Game/TerritoryMode.cpp
// RS2V Territory game mode implementation

#include "Game/TerritoryMode.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include <algorithm>

TerritoryMode::TerritoryMode(GameServer* server)
    : m_server(server)
{
}

TerritoryMode::~TerritoryMode() {
    Shutdown();
}

void TerritoryMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_currentRound = 0;
    m_phaseTimer = 0.0f;
    m_objectivesCapturedRound[0] = 0;
    m_objectivesCapturedRound[1] = 0;
    Logger::Info("TerritoryMode initialized");
}

void TerritoryMode::Shutdown() {
    Logger::Info("TerritoryMode shutdown");
}

void TerritoryMode::StartRound() {
    SetPhase(Phase::Preparation);
    SetupObjectives();

    // Initialize tickets via TicketSystem (accessed through GameServer)
    m_objectivesCapturedRound[m_currentRound] = 0;

    Logger::Info("Territory round %d started — Team %u attacking, Team %u defending",
                 m_currentRound + 1, m_attackingTeam, m_defendingTeam);
}

void TerritoryMode::EndRound() {
    // Snapshot ticket counts for tiebreaker
    auto* tm = m_server->GetTeamManager();
    m_ticketsRemainingRound[m_currentRound][0] = tm->GetTeamScore(m_attackingTeam);
    m_ticketsRemainingRound[m_currentRound][1] = tm->GetTeamScore(m_defendingTeam);

    Logger::Info("Territory round %d ended — Attackers captured %d objectives",
                 m_currentRound + 1, m_objectivesCapturedRound[m_currentRound]);

    if (m_currentRound == 0) {
        // First round done — go to halftime
        SetPhase(Phase::HalfTime);
    } else {
        // Both rounds done — determine overall winner
        SetPhase(Phase::Finished);
        DetermineWinner();
    }
}

void TerritoryMode::SwitchSides() {
    std::swap(m_attackingTeam, m_defendingTeam);
    m_currentRound = 1;
    Logger::Info("Sides switched — Team %u now attacking, Team %u defending",
                 m_attackingTeam, m_defendingTeam);
    StartRound();
}

void TerritoryMode::Update(float deltaSeconds) {
    m_phaseTimer -= deltaSeconds;

    switch (m_phase) {
        case Phase::WarmUp:
            // Wait for enough players
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
                // Main timer expired — check if overtime or lockdown
                if (AreAttackersContesting()) {
                    SetPhase(Phase::Overtime);
                } else {
                    SetPhase(Phase::Lockdown);
                }
            }
            break;

        case Phase::Overtime:
            CheckOvertimeConditions();
            if (!AreAttackersContesting() || m_phaseTimer <= 0.0f) {
                // Overtime ends — defenders win
                Logger::Info("Overtime ended — defenders hold");
                EndRound();
            }
            break;

        case Phase::Lockdown:
            CheckWinConditions();
            if (m_phaseTimer <= 0.0f) {
                // Lockdown expired — defenders win
                Logger::Info("Lockdown expired — defenders hold");
                EndRound();
            }
            break;

        case Phase::SuddenDeath:
            // No timer — wait for last team standing
            break;

        case Phase::PostRound:
            if (m_phaseTimer <= 0.0f) {
                EndRound();
            }
            break;

        case Phase::HalfTime:
            if (m_phaseTimer <= 0.0f) {
                SwitchSides();
            }
            break;

        case Phase::Finished:
            // Game over — server will handle map change
            break;
    }
}

void TerritoryMode::OnObjectiveCaptured(uint32_t /*objectiveId*/, uint32_t capturingTeam) {
    if (capturingTeam != m_attackingTeam) return;

    m_objectivesCapturedRound[m_currentRound]++;

    // Replenish attacker tickets
    Logger::Info("Territory objective captured by attackers — +%u tickets", m_ticketsOnCapture);

    m_server->BroadcastChatMessage("[Territory] Objective captured! Attackers reinforced.");
}

void TerritoryMode::OnTicketsDepleted(uint32_t teamId) {
    Logger::Info("Team %u tickets depleted — entering sudden death", teamId);
    SetPhase(Phase::SuddenDeath);
}

void TerritoryMode::OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {
    // Kill tracking is handled by DamageSystem/TicketSystem
    // Check sudden death conditions here
    if (m_phase == Phase::SuddenDeath) {
        // Check if one side is completely eliminated
        auto* pm = m_server->GetPlayerManager();
        auto attackerPlayers = m_server->GetTeamManager()->GetTeamPlayers(m_attackingTeam);
        auto defenderPlayers = m_server->GetTeamManager()->GetTeamPlayers(m_defendingTeam);

        bool attackersAlive = false;
        bool defendersAlive = false;

        for (uint32_t pid : attackerPlayers) {
            auto p = pm->GetPlayer(pid);
            if (p && p->IsAlive()) { attackersAlive = true; break; }
        }
        for (uint32_t pid : defenderPlayers) {
            auto p = pm->GetPlayer(pid);
            if (p && p->IsAlive()) { defendersAlive = true; break; }
        }

        if (!attackersAlive) {
            Logger::Info("All attackers eliminated — defenders win");
            EndRound();
        } else if (!defendersAlive) {
            Logger::Info("All defenders eliminated — attackers win");
            EndRound();
        }
    }
}

void TerritoryMode::OnAllPlayersDead(uint32_t teamId) {
    if (m_phase == Phase::SuddenDeath) {
        if (teamId == m_attackingTeam) {
            Logger::Info("Attackers eliminated in sudden death — defenders win");
        } else {
            Logger::Info("Defenders eliminated in sudden death — attackers win");
        }
        EndRound();
    }
}

float TerritoryMode::GetRoundTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float TerritoryMode::GetOvertimeRemaining() const {
    return m_phase == Phase::Overtime ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

int TerritoryMode::GetAttackerObjectivesCaptured(int round) const {
    if (round < 0 || round > 1) return 0;
    return m_objectivesCapturedRound[round];
}

int TerritoryMode::GetRoundTicketsRemaining(int round, uint32_t teamId) const {
    if (round < 0 || round > 1) return 0;
    if (teamId == m_attackingTeam) return m_ticketsRemainingRound[round][0];
    if (teamId == m_defendingTeam) return m_ticketsRemainingRound[round][1];
    return 0;
}

void TerritoryMode::SetRoundTime(float seconds) { m_roundTime = seconds; }
void TerritoryMode::SetLockdownTime(float seconds) { m_lockdownTime = seconds; }
void TerritoryMode::SetPreparationTime(float seconds) { m_preparationTime = seconds; }
void TerritoryMode::SetPostRoundTime(float seconds) { m_postRoundTime = seconds; }
void TerritoryMode::SetAttackerTickets(uint32_t tickets) { m_attackerStartTickets = tickets; }
void TerritoryMode::SetDefenderTickets(uint32_t tickets) { m_defenderStartTickets = tickets; }
void TerritoryMode::SetTicketsOnCapture(uint32_t tickets) { m_ticketsOnCapture = tickets; }

void TerritoryMode::SetPhase(Phase newPhase) {
    m_phase = newPhase;
    switch (newPhase) {
        case Phase::WarmUp:       m_phaseTimer = 0.0f; break;
        case Phase::Preparation:  m_phaseTimer = m_preparationTime; break;
        case Phase::Active:       m_phaseTimer = m_roundTime; break;
        case Phase::Overtime:     m_phaseTimer = m_overtimeMaxTime; break;
        case Phase::Lockdown:     m_phaseTimer = m_lockdownTime; break;
        case Phase::SuddenDeath:  m_phaseTimer = 0.0f; break;  // No timer
        case Phase::PostRound:    m_phaseTimer = m_postRoundTime; break;
        case Phase::HalfTime:     m_phaseTimer = 15.0f; break;
        case Phase::Finished:     m_phaseTimer = 0.0f; break;
    }
    BroadcastPhaseChange();
    Logger::Info("TerritoryMode phase: %d, timer: %.1fs", static_cast<int>(newPhase), m_phaseTimer);
}

void TerritoryMode::CheckWinConditions() {
    // Attackers win by capturing all objectives (checked via ObjectiveSystem callback)
    // Defenders win by time running out or eliminating all attackers + tickets
}

void TerritoryMode::CheckOvertimeConditions() {
    if (!AreAttackersContesting()) {
        // Attackers left the objective — overtime ends immediately
        Logger::Info("Attackers no longer contesting — overtime ends");
        EndRound();
    }
}

void TerritoryMode::DetermineWinner() {
    // Tiebreaker: most objectives captured, then most tickets remaining
    int r0Obj = m_objectivesCapturedRound[0];
    int r1Obj = m_objectivesCapturedRound[1];

    if (r0Obj > r1Obj) {
        Logger::Info("Round 1 attackers (now team %u) win by objectives: %d vs %d",
                     m_defendingTeam, r0Obj, r1Obj);
    } else if (r1Obj > r0Obj) {
        Logger::Info("Round 2 attackers (team %u) win by objectives: %d vs %d",
                     m_attackingTeam, r1Obj, r0Obj);
    } else {
        // Ticket tiebreaker
        Logger::Info("Tied on objectives (%d each) — using ticket tiebreaker", r0Obj);
    }

    m_server->BroadcastChatMessage("[Territory] Match complete!");
}

bool TerritoryMode::AreAttackersContesting() const {
    // Check if any attacker is inside an active objective zone
    // This would query the ObjectiveSystem
    return false;  // Placeholder — integrated via ObjectiveSystem
}

void TerritoryMode::SetupObjectives() {
    // Objectives are loaded from map data and registered with ObjectiveSystem
    Logger::Info("Territory objectives setup for round %d", m_currentRound + 1);
}

void TerritoryMode::BroadcastPhaseChange() const {
    std::string msg;
    switch (m_phase) {
        case Phase::WarmUp:       msg = "Waiting for players..."; break;
        case Phase::Preparation:  msg = "Round starting soon!"; break;
        case Phase::Active:       msg = "Round is active!"; break;
        case Phase::Overtime:     msg = "OVERTIME — Attackers contesting!"; break;
        case Phase::Lockdown:     msg = "Lockdown period — Attackers must push!"; break;
        case Phase::SuddenDeath:  msg = "SUDDEN DEATH — No respawns!"; break;
        case Phase::PostRound:    msg = "Round over!"; break;
        case Phase::HalfTime:     msg = "Halftime — Switching sides..."; break;
        case Phase::Finished:     msg = "Match complete!"; break;
    }
    m_server->BroadcastChatMessage("[Territory] " + msg);
}
