// src/Game/TerritoryMode.cpp
// RS2V Territory game mode implementation

#include "Game/TerritoryMode.h"
#include "Game/BotManager.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/ObjectiveSystem.h"
#include "Game/TicketSystem.h"
#include "Game/TeamMapping.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <limits>

TerritoryMode::TerritoryMode(GameServer* server)
    : m_server(server)
{
}

TerritoryMode::~TerritoryMode() {
    Shutdown();
}

void TerritoryMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_attackingTeam = TeamMapping::kServerUs;
    m_defendingTeam = TeamMapping::kServerNva;
    m_currentRound = 0;
    m_phaseTimer = 0.0f;
    m_winningTeam = 0;
    m_roundFinalized = false;
    m_capturedObjectiveIds.clear();
    ClearLockdownSchedule();
    for (int round = 0; round < 2; ++round) {
        m_objectivesCapturedRound[round] = 0;
        m_attackingTeamRound[round] = 0;
        m_ticketsRemainingRound[round][0] = 0;
        m_ticketsRemainingRound[round][1] = 0;
    }
    Logger::Info("TerritoryMode initialized");
}

void TerritoryMode::Shutdown() {
    Logger::Info("TerritoryMode shutdown");
}

void TerritoryMode::StartRound() {
    if ((m_phase != Phase::WarmUp && m_phase != Phase::HalfTime) ||
        m_currentRound < 0 || m_currentRound > 1) {
        Logger::Warn("Territory round start ignored from phase %d/round %d",
                     static_cast<int>(m_phase), m_currentRound);
        return;
    }

    m_roundFinalized = false;
    m_capturedObjectiveIds.clear();
    SetPhase(Phase::Preparation);
    SetupObjectives();
    m_attackingTeamRound[m_currentRound] = m_attackingTeam;

    // Apply role-relative ticket counts to the current numeric teams. This also
    // swaps the starting pools correctly at halftime.
    if (m_server) {
        if (auto* tickets = m_server->GetTicketSystem()) {
            const uint32_t team1Tickets = m_attackingTeam == 1
                ? m_attackerStartTickets : m_defenderStartTickets;
            const uint32_t team2Tickets = m_attackingTeam == 2
                ? m_attackerStartTickets : m_defenderStartTickets;
            tickets->Initialize(team1Tickets, team2Tickets);
        }
    }
    m_objectivesCapturedRound[m_currentRound] = 0;

    Logger::Info("Territory round %d started — Team %u attacking, Team %u defending",
                 m_currentRound + 1, m_attackingTeam, m_defendingTeam);
}

void TerritoryMode::EndRound() {
    if (m_roundFinalized || m_phase != Phase::PostRound ||
        m_currentRound < 0 || m_currentRound > 1) {
        return;
    }
    m_roundFinalized = true;

    // Snapshot authoritative reinforcement pools by stable numeric server team.
    // Team scores are unrelated, and role-relative slots become ambiguous as
    // soon as halftime swaps attacker and defender.
    if (m_server) {
        if (auto* tickets = m_server->GetTicketSystem()) {
            m_ticketsRemainingRound[m_currentRound][0] =
                tickets->GetTickets(TeamMapping::kServerUs);
            m_ticketsRemainingRound[m_currentRound][1] =
                tickets->GetTickets(TeamMapping::kServerNva);
        }
    }

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
    if (m_phase != Phase::HalfTime || m_currentRound != 0) return;

    std::swap(m_attackingTeam, m_defendingTeam);
    m_currentRound = 1;
    Logger::Info("Sides switched — Team %u now attacking, Team %u defending",
                 m_attackingTeam, m_defendingTeam);
    StartRound();
}

void TerritoryMode::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return;

    // Active owns its countdown because retail Territory lockdown is an
    // absolute target on the decreasing round clock, not another phase added
    // after that clock expires. Keeping the subtraction in one place also lets
    // a large frame cross the inactivity and lockdown boundaries atomically.
    if (m_phase == Phase::Active) {
        AdvanceActivePhase(deltaSeconds,
                           AreAttackersContesting(),
                           HasEligibleLockdownObjective());
        return;
    }

    m_phaseTimer = std::max(0.0f, m_phaseTimer - deltaSeconds);

    switch (m_phase) {
        case Phase::WarmUp:
            // Wait for enough players
            if (m_server && m_server->GetTeamManager() &&
                m_server->GetTeamManager()->HasEnoughPlayers()) {
                StartRound();
            }
            break;

        case Phase::Preparation:
            if (m_phaseTimer <= 0.0f) {
                SetPhase(Phase::Active);
            }
            break;

        case Phase::Active:
            // Handled above so the active clock is decremented exactly once.
            break;

        case Phase::Overtime:
            CheckWinConditions();
            if (m_phase != Phase::Overtime) break;
            if (!AreAttackersContesting() || m_phaseTimer <= 0.0f) {
                // Overtime ends — defenders win
                Logger::Info("Overtime ended — defenders hold");
                SetPhase(Phase::PostRound);
            }
            break;

        case Phase::Lockdown:
            CheckWinConditions();
            if (m_phase != Phase::Lockdown) break;
            if (m_phaseTimer <= 0.0f) {
                // Lockdown expired — defenders win
                Logger::Info("Lockdown expired — defenders hold");
                SetPhase(Phase::PostRound);
            }
            break;

        case Phase::SuddenDeath:
            // Bot deaths are headless and do not carry a player id into the
            // ordinary kill callback. Poll the combined roster each tick.
            CheckSuddenDeathElimination();
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

void TerritoryMode::OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam) {
    if (!CanCaptureObjectives() || capturingTeam != m_attackingTeam || !m_server ||
        m_currentRound < 0 || m_currentRound > 1) {
        return;
    }

    auto* objectives = m_server->GetObjectiveSystem();
    const CaptureZone* zone = objectives ? objectives->GetObjective(objectiveId) : nullptr;
    if (!zone || !zone->enabled || zone->type != ObjectiveType::Territory ||
        zone->controllingTeam != m_attackingTeam) {
        Logger::Warn("Ignoring invalid Territory capture event for objective %u/team %u",
                     objectiveId, capturingTeam);
        return;
    }
    if (!m_capturedObjectiveIds.insert(objectiveId).second) return;

    m_objectivesCapturedRound[m_currentRound] = static_cast<int>(std::min(
        m_capturedObjectiveIds.size(),
        static_cast<size_t>(std::numeric_limits<int>::max())));

    // Replenish attacker tickets
    Logger::Info("Territory objective captured by attackers — +%u tickets", m_ticketsOnCapture);
    if (auto* tickets = m_server->GetTicketSystem()) {
        tickets->AddTickets(m_attackingTeam, m_ticketsOnCapture);
    }

    m_server->BroadcastChatMessage("[Territory] Objective captured! Attackers reinforced.");

    // ObjectiveSystem invokes this callback after assigning ownership and
    // advancing the Territory chain. End the live round when the attacker
    // owns the whole chain; the old empty CheckWinConditions left a captured
    // Hotel running until the clock expired.
    if (objectives->GetObjectiveCount() > 0) {
        if (objectives->AreAllObjectivesCapturedBy(m_attackingTeam)) {
            Logger::Info("Territory attackers captured every objective; entering post-round");
            m_server->BroadcastChatMessage("[Territory] All objectives captured!");
            SetPhase(Phase::PostRound);
            return;
        }
    }

    // A completed capture starts both retail inactivity clocks over against
    // the newly active phase. Unknown or malformed objective metadata disables
    // only the early-win schedule; it never changes the main round deadline.
    if (m_phase == Phase::Active) {
        SynchronizeLockdownEligibility(HasEligibleLockdownObjective(),
                                       std::max(0.0f, m_phaseTimer),
                                       true);
    }
}

void TerritoryMode::AdvanceActivePhase(float deltaSeconds,
                                       bool attackersContesting,
                                       bool lockdownObjectiveEligible) {
    if (m_phase != Phase::Active || !std::isfinite(deltaSeconds) ||
        deltaSeconds < 0.0f) {
        return;
    }

    CheckWinConditions();
    if (m_phase != Phase::Active) return;

    const float previousRemaining = std::max(0.0f, m_phaseTimer);
    SynchronizeLockdownEligibility(lockdownObjectiveEligible,
                                   previousRemaining);
    const float nextRemaining =
        std::max(0.0f, previousRemaining - deltaSeconds);

    // Retail updates LastCaptureAttempt when a new push begins. A continuing
    // contest must not slide the estimated deadline every frame, and once the
    // actual target is armed a later attempt cannot move it.
    if (m_lockdownObjectiveEligible && m_nextLockdownTime < 0.0f &&
        attackersContesting && !m_attackersWereContesting) {
        m_lastCaptureAttempt = nextRemaining;
        RecomputeEstimatedLockdown();
    }

    if (m_lockdownObjectiveEligible && m_nextLockdownTime < 0.0f) {
        const float captureTrigger = m_lastCaptureTime >= 0.0f
            ? m_lastCaptureTime - m_captureLockdownDelay
            : -1.0f;
        const float attemptTrigger = m_lastCaptureAttempt >= 0.0f
            ? m_lastCaptureAttempt - m_captureAttemptLockdownDelay
            : -1.0f;
        const float inactivityTrigger =
            std::max(captureTrigger, attemptTrigger);

        // The retail one-second loop only arms a target when the complete
        // LockdownTime interval still fits above zero. Store the analytically
        // exact target so coarse native frames cannot skip the early win.
        if (inactivityTrigger > 0.0f &&
            nextRemaining <= inactivityTrigger &&
            m_nextEstimatedLockdownTime > 0.0f) {
            m_nextLockdownTime = m_nextEstimatedLockdownTime;
            m_nextEstimatedLockdownTime = -1.0f;
            Logger::Info("Territory lockdown target armed at %.1fs remaining",
                         m_nextLockdownTime);
        }
    }

    m_attackersWereContesting =
        m_lockdownObjectiveEligible && attackersContesting;
    m_phaseTimer = nextRemaining;

    if (ResolveEarlyLockdown(attackersContesting)) return;

    if (m_phaseTimer <= 0.0f) {
        if (attackersContesting) {
            Logger::Info("Territory main timer expired while attackers contest; entering overtime");
            SetPhase(Phase::Overtime);
        } else {
            Logger::Info("Territory main timer expired; defenders hold");
            SetPhase(Phase::PostRound);
        }
    }
}

void TerritoryMode::SynchronizeLockdownEligibility(bool eligible,
                                                   float baselineRemaining,
                                                   bool forceReset) {
    if (!eligible || !std::isfinite(baselineRemaining) ||
        baselineRemaining < 0.0f) {
        ClearLockdownSchedule();
        return;
    }

    if (!m_lockdownObjectiveEligible || forceReset) {
        m_lockdownObjectiveEligible = true;
        m_lastCaptureTime = baselineRemaining;
        m_lastCaptureAttempt = baselineRemaining;
        m_nextLockdownTime = -1.0f;
        m_attackersWereContesting = false;
        RecomputeEstimatedLockdown();
    }
}

void TerritoryMode::ClearLockdownSchedule() {
    m_lastCaptureTime = -1.0f;
    m_lastCaptureAttempt = -1.0f;
    m_nextLockdownTime = -1.0f;
    m_nextEstimatedLockdownTime = -1.0f;
    m_lockdownObjectiveEligible = false;
    m_attackersWereContesting = false;
}

void TerritoryMode::RecomputeEstimatedLockdown() {
    m_nextEstimatedLockdownTime = -1.0f;
    if (!m_lockdownObjectiveEligible || m_nextLockdownTime >= 0.0f) return;

    float inactivityTrigger = -1.0f;
    if (m_lastCaptureTime >= 0.0f) {
        inactivityTrigger =
            std::max(inactivityTrigger,
                     m_lastCaptureTime - m_captureLockdownDelay);
    }
    if (m_lastCaptureAttempt >= 0.0f) {
        inactivityTrigger =
            std::max(inactivityTrigger,
                     m_lastCaptureAttempt - m_captureAttemptLockdownDelay);
    }

    const float estimatedTarget = inactivityTrigger - m_lockdownTime;
    if (inactivityTrigger > 0.0f && estimatedTarget > 0.0f &&
        std::isfinite(estimatedTarget)) {
        m_nextEstimatedLockdownTime = estimatedTarget;
    }
}

bool TerritoryMode::ResolveEarlyLockdown(bool attackersContesting) {
    if (m_phase != Phase::Active || !m_lockdownObjectiveEligible ||
        m_nextLockdownTime < 0.0f || m_phaseTimer > m_nextLockdownTime) {
        return false;
    }

    if (attackersContesting) {
        Logger::Info("Territory lockdown target reached while attackers contest; entering overtime");
        SetPhase(Phase::Overtime);
    } else {
        Logger::Info("Territory lockdown target reached; defenders hold");
        SetPhase(Phase::PostRound);
    }
    return true;
}

void TerritoryMode::OnTicketsDepleted(uint32_t teamId) {
    if ((m_phase != Phase::Active && m_phase != Phase::Overtime &&
         m_phase != Phase::Lockdown) ||
        (teamId != m_attackingTeam && teamId != m_defendingTeam)) {
        return;
    }
    Logger::Info("Team %u tickets depleted — entering sudden death", teamId);
    SetPhase(Phase::SuddenDeath);
}

void TerritoryMode::OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {
    // Kill tracking is handled by DamageSystem/TicketSystem. Elimination must
    // still include headless bots, which are not represented by player ids.
    CheckSuddenDeathElimination();
}

void TerritoryMode::OnAllPlayersDead(uint32_t teamId) {
    if (teamId != m_attackingTeam && teamId != m_defendingTeam) return;
    // This callback describes the visible player roster only. Re-check the
    // unified roster rather than ending a round while a bot remains alive.
    CheckSuddenDeathElimination();
}

float TerritoryMode::GetRoundTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float TerritoryMode::GetPhaseDuration() const {
    switch (m_phase) {
        case Phase::WarmUp:       return 0.0f;
        case Phase::Preparation:  return m_preparationTime;
        case Phase::Active:       return m_roundTime;
        case Phase::Overtime:     return m_overtimeMaxTime;
        case Phase::Lockdown:     return m_lockdownTime;
        case Phase::SuddenDeath:  return 0.0f;
        case Phase::PostRound:    return m_postRoundTime;
        case Phase::HalfTime:     return 15.0f;
        case Phase::Finished:     return 0.0f;
    }
    return 0.0f;
}

float TerritoryMode::GetOvertimeRemaining() const {
    return m_phase == Phase::Overtime ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

TerritoryMode::RetailTimingState TerritoryMode::GetRetailTimingState() const {
    RetailTimingState state;
    state.overtime = m_phase == Phase::Overtime;
    if (m_phase == Phase::Active && m_lockdownObjectiveEligible) {
        state.nextLockdownTime = ToRetailCountdown(m_nextLockdownTime);
        state.nextEstimatedLockdownTime =
            ToRetailCountdown(m_nextEstimatedLockdownTime);
    }
    return state;
}

int TerritoryMode::GetAttackerObjectivesCaptured(int round) const {
    if (round < 0 || round > 1) return 0;
    return m_objectivesCapturedRound[round];
}

uint32_t TerritoryMode::GetRoundTicketsRemaining(int round, uint32_t teamId) const {
    if (round < 0 || round > 1) return 0u;
    if (teamId == TeamMapping::kServerUs) {
        return m_ticketsRemainingRound[round][0];
    }
    if (teamId == TeamMapping::kServerNva) {
        return m_ticketsRemainingRound[round][1];
    }
    return 0u;
}

void TerritoryMode::SetRoundTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) m_roundTime = seconds;
}
void TerritoryMode::SetLockdownTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) m_lockdownTime = seconds;
}
void TerritoryMode::SetCaptureLockdownDelay(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) {
        m_captureLockdownDelay = seconds;
    }
}
void TerritoryMode::SetCaptureAttemptLockdownDelay(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) {
        m_captureAttemptLockdownDelay = seconds;
    }
}
void TerritoryMode::SetPreparationTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) m_preparationTime = seconds;
}
void TerritoryMode::SetPostRoundTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) m_postRoundTime = seconds;
}
void TerritoryMode::SetAttackerTickets(uint32_t tickets) { m_attackerStartTickets = tickets; }
void TerritoryMode::SetDefenderTickets(uint32_t tickets) { m_defenderStartTickets = tickets; }
void TerritoryMode::SetTicketsOnCapture(uint32_t tickets) { m_ticketsOnCapture = tickets; }

void TerritoryMode::SetPhase(Phase newPhase) {
    if (m_phase == newPhase) return;
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
    if (newPhase == Phase::Active) {
        SynchronizeLockdownEligibility(HasEligibleLockdownObjective(),
                                       std::max(0.0f, m_phaseTimer),
                                       true);
    } else {
        ClearLockdownSchedule();
    }
    BroadcastPhaseChange();
    Logger::Info("TerritoryMode phase: %d, timer: %.1fs", static_cast<int>(newPhase), m_phaseTimer);
}

void TerritoryMode::CheckWinConditions() {
    if (!CanCaptureObjectives() || !m_server) return;
    auto* objectives = m_server->GetObjectiveSystem();
    if (!objectives || objectives->GetObjectiveCount() == 0) return;
    if (objectives->AreAllObjectivesCapturedBy(m_attackingTeam)) {
        Logger::Info("Territory attackers own every enabled objective; entering post-round");
        SetPhase(Phase::PostRound);
    }
}

bool TerritoryMode::TeamHasLivingParticipant(uint32_t teamId) const {
    if (!m_server ||
        (teamId != m_attackingTeam && teamId != m_defendingTeam)) {
        return false;
    }

    auto* players = m_server->GetPlayerManager();
    auto* teams = m_server->GetTeamManager();
    if (players && teams) {
        for (const uint32_t playerId : teams->GetTeamPlayers(teamId)) {
            const auto player = players->GetPlayer(playerId);
            if (player && player->IsAlive()) return true;
        }
    }

    if (const auto* bots = m_server->GetBotManager()) {
        return bots->CountAliveBots(static_cast<uint8_t>(teamId)) > 0;
    }
    return false;
}

void TerritoryMode::CheckSuddenDeathElimination() {
    if (m_phase != Phase::SuddenDeath || !m_server) return;

    const bool attackersAlive = TeamHasLivingParticipant(m_attackingTeam);
    const bool defendersAlive = TeamHasLivingParticipant(m_defendingTeam);
    if (!attackersAlive && !defendersAlive) {
        Logger::Info("Both teams eliminated in Territory sudden death");
        SetPhase(Phase::PostRound);
    } else if (!attackersAlive) {
        Logger::Info("All attackers eliminated - defenders hold");
        SetPhase(Phase::PostRound);
    } else if (!defendersAlive) {
        Logger::Info("All defenders eliminated - attackers advance");
        SetPhase(Phase::PostRound);
    }
}

void TerritoryMode::DetermineWinner() {
    // Tiebreaker: most objectives captured, then most tickets remaining
    int r0Obj = m_objectivesCapturedRound[0];
    int r1Obj = m_objectivesCapturedRound[1];

    if (r0Obj > r1Obj) {
        m_winningTeam = m_attackingTeamRound[0];
        Logger::Info("Round 1 attackers (now team %u) win by objectives: %d vs %d",
                     m_winningTeam, r0Obj, r1Obj);
    } else if (r1Obj > r0Obj) {
        m_winningTeam = m_attackingTeamRound[1];
        Logger::Info("Round 2 attackers (team %u) win by objectives: %d vs %d",
                     m_winningTeam, r1Obj, r0Obj);
    } else {
        const uint32_t round0Attacker = m_attackingTeamRound[0];
        const uint32_t round1Attacker = m_attackingTeamRound[1];
        const uint32_t round0Tickets = GetRoundTicketsRemaining(0, round0Attacker);
        const uint32_t round1Tickets = GetRoundTicketsRemaining(1, round1Attacker);
        if (round0Tickets > round1Tickets) {
            m_winningTeam = round0Attacker;
        } else if (round1Tickets > round0Tickets) {
            m_winningTeam = round1Attacker;
        } else {
            m_winningTeam = 0;
        }
        Logger::Info("Tied on objectives (%d each); attacker ticket tiebreaker "
                     "round1=%u round2=%u winner=%u",
                     r0Obj, round0Tickets, round1Tickets, m_winningTeam);
    }

}

bool TerritoryMode::AreAttackersContesting() const {
    // Attackers are contesting if any player on the attacking team is currently
    // inside an active objective's capture zone. Query the ObjectiveSystem,
    // which refreshes per-zone player presence each tick. Note that a zone's
    // attackerIds/defenderIds are relative to that zone's controlling team, not
    // to the Territory attacking team, so check zone membership by team here.
    if (!m_server) return false;

    auto* objSys = m_server->GetObjectiveSystem();
    auto* teamMgr = m_server->GetTeamManager();
    if (!objSys || !teamMgr) return false;

    return AnyAttackerInCurrentTerritoryPhase(
        *objSys,
        m_attackingTeam,
        [teamMgr, this](uint32_t playerId) {
            return teamMgr->GetPlayerTeam(playerId) == m_attackingTeam;
        });
}

bool TerritoryMode::HasEligibleLockdownObjective() const {
    if (!m_server) return false;
    const auto* objectives = m_server->GetObjectiveSystem();
    return objectives && CurrentTerritoryPhaseSupportsLockdown(
                             *objectives, m_defendingTeam);
}

bool TerritoryMode::CurrentTerritoryPhaseSupportsLockdown(
    const ObjectiveSystem& objectives,
    uint32_t defendingTeam) {
    if (defendingTeam != TeamMapping::kServerUs &&
        defendingTeam != TeamMapping::kServerNva) {
        return false;
    }

    const CaptureZone* current = objectives.GetCurrentTerritoryObjective();
    if (!current || !current->enabled || !current->isActive ||
        current->type != ObjectiveType::Territory ||
        current->controllingTeam != defendingTeam) {
        return false;
    }

    const int currentPhase = current->territoryOrder;
    bool foundCurrentPhaseObjective = false;
    bool lockdownEnabled = false;
    for (const CaptureZone* zone : objectives.GetActiveObjectives()) {
        if (!zone || zone->type != ObjectiveType::Territory ||
            zone->territoryOrder != currentPhase) {
            continue;
        }

        foundCurrentPhaseObjective = true;
        if (zone->controllingTeam != defendingTeam ||
            !zone->lockdownMetadataKnown ||
            std::any_of(zone->lockdownTimeSecondsByPlayerBand.begin(),
                        zone->lockdownTimeSecondsByPlayerBand.end(),
                        [](int32_t seconds) { return seconds < 0; })) {
            // Unknown or malformed cooked metadata must not invent a retail
            // early-win rule. The ordinary round clock remains authoritative.
            return false;
        }
        lockdownEnabled = lockdownEnabled || zone->lockdownEnabled;
    }

    return foundCurrentPhaseObjective && lockdownEnabled;
}

int32_t TerritoryMode::ToRetailCountdown(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0f) return -1;
    const double rounded = std::ceil(static_cast<double>(seconds));
    if (rounded > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(rounded);
}

bool TerritoryMode::AnyAttackerInCurrentTerritoryPhase(
    const ObjectiveSystem& objectives,
    uint32_t attackingTeam,
    const std::function<bool(uint32_t)>& isAttackingPlayer) {
    // A Territory phase can contain several simultaneous objectives (Resort's
    // Villa/Farm pair). GetCurrentTerritoryObjective() identifies that phase;
    // then inspect every active, enabled sibling in it. Filtering by the phase
    // remains important even if malformed/runtime state marks a future zone
    // active, because that zone must not prolong the current phase's overtime.
    const CaptureZone* current = objectives.GetCurrentTerritoryObjective();
    if (!current || !isAttackingPlayer) return false;

    const int currentPhase = current->territoryOrder;
    for (const CaptureZone* zone : objectives.GetActiveObjectives()) {
        if (!zone || !zone->enabled || zone->territoryOrder != currentPhase) {
            continue;
        }

        if (objectives.GetBotCaptureWeight(zone->id, attackingTeam) > 0.0f) {
            return true;
        }

        for (uint32_t playerId : zone->attackerIds) {
            if (isAttackingPlayer(playerId)) return true;
        }
        for (uint32_t playerId : zone->defenderIds) {
            if (isAttackingPlayer(playerId)) return true;
        }
    }
    return false;
}

void TerritoryMode::SetupObjectives() {
    // Objective actors are loaded once per map, but ownership, progress and the
    // active phase are per-round. Without this reset the second half begins with
    // Beach already captured and the chain still parked at the prior round's end.
    if (m_server) {
        if (auto* objectives = m_server->GetObjectiveSystem()) {
            objectives->ResetTerritoryForRound(m_defendingTeam);
        }
    }
    Logger::Info("Territory objectives reset for round %d (defender team %u)",
                 m_currentRound + 1, m_defendingTeam);
}

void TerritoryMode::BroadcastPhaseChange() const {
    if (!m_server) return;

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
