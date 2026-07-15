// src/Game/SkirmishMode.cpp
// RS2V Skirmish game mode implementation.

#include "Game/SkirmishMode.h"

#include "Game/BotManager.h"
#include "Game/GameServer.h"
#include "Game/ObjectiveSystem.h"
#include "Game/PlayerManager.h"
#include "Game/SpawnSystem.h"
#include "Game/TeamManager.h"
#include "Game/TicketSystem.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace {

int32_t ToRetailCountdown(float seconds) {
    if (!(seconds > 0.0f)) return 0;
    const double rounded = std::ceil(static_cast<double>(seconds));
    return static_cast<int32_t>(std::min(
        rounded, static_cast<double>(std::numeric_limits<int32_t>::max())));
}

uint8_t ToRetailAliveCount(std::size_t count) {
    return static_cast<uint8_t>(std::min(
        count, static_cast<std::size_t>(std::numeric_limits<uint8_t>::max())));
}

} // namespace

SkirmishMode::SkirmishMode(GameServer* server)
    : m_server(server)
{
}

SkirmishMode::~SkirmishMode() {
    Shutdown();
}

void SkirmishMode::Initialize() {
    m_phase = Phase::WarmUp;
    m_phaseTimer = 0.0f;
    m_currentRound = 0;
    m_playedRoundsCount = 0;
    m_team1Wins = 0;
    m_team2Wins = 0;
    m_roundWinner = 0;
    m_roundRecorded = false;
    ResetRoundRetailState();
    RefreshAliveCounts();
    Logger::Info("SkirmishMode initialized (%d rounds, %d waves at %ds, %ds lockdown)",
                 m_maxRounds, kSpawnWaveCount, kSpawnWaveIntervalSeconds,
                 kAllObjectiveLockdownSeconds);
}

void SkirmishMode::Shutdown() {
    Logger::Info("SkirmishMode shutdown");
}

void SkirmishMode::StartRound() {
    if (m_phase != Phase::WarmUp && m_phase != Phase::NextRound) {
        Logger::Warn("Skirmish round start ignored from phase %d",
                     static_cast<int>(m_phase));
        return;
    }
    if (m_currentRound >= m_maxRounds) {
        DetermineMatchWinner();
        SetPhase(Phase::Finished);
        return;
    }

    ++m_currentRound;
    m_roundWinner = 0;
    m_roundRecorded = false;
    ResetRoundRetailState();
    if (m_server) {
        if (auto* objectives = m_server->GetObjectiveSystem()) {
            objectives->ResetObjectivesToInitialOwners();
        }
    }
    OpenInitialSpawnWindows();
    RefreshAliveCounts();
    SetPhase(Phase::Preparation);
    Logger::Info("Skirmish round %d/%d starting", m_currentRound, m_maxRounds);
}

void SkirmishMode::EndRound() {
    if (m_phase != Phase::PostRound) return;

    if (!m_roundRecorded && m_currentRound > 0) {
        ++m_playedRoundsCount;
        m_roundRecorded = true;
    }

    Logger::Info("Skirmish round %d ended (Team1: %d wins, Team2: %d wins)",
                 m_currentRound, m_team1Wins, m_team2Wins);

    const int winsNeeded = (m_maxRounds / 2) + 1;
    if (m_team1Wins >= winsNeeded || m_team2Wins >= winsNeeded ||
        m_currentRound >= m_maxRounds) {
        DetermineMatchWinner();
        SetPhase(Phase::Finished);
    } else {
        SetPhase(Phase::NextRound);
    }
}

void SkirmishMode::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return;
    if (m_phase == Phase::Finished) return;

    const float elapsed = deltaSeconds;
    m_phaseTimer = std::max(0.0f, m_phaseTimer - elapsed);
    RefreshAliveCounts();

    switch (m_phase) {
        case Phase::WarmUp: {
            auto* teams = m_server ? m_server->GetTeamManager() : nullptr;
            if (teams && teams->HasEnoughPlayers()) {
                StartRound();
            }
            break;
        }

        case Phase::Preparation:
            if (m_phaseTimer <= 0.0f) {
                SetPhase(Phase::Active);
            }
            break;

        case Phase::Active:
            RefreshObjectiveState();
            if (CheckObjectiveLockdown()) break;
            CheckWinConditions();
            if (m_phase != Phase::Active) break;
            if (m_phaseTimer <= 0.0f) {
                if (m_overtime && m_objectiveContested) {
                    m_enteredSuddenDeathInOvertime = true;
                }
                BeginSuddenDeath();
            }
            break;

        case Phase::InstantDeath:
            RefreshObjectiveState();
            CheckInstantDeathWinner();
            if (m_phase != Phase::InstantDeath) break;
            CheckObjectiveLockdown();
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

void SkirmishMode::OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam) {
    if (!CanCaptureObjectives() || !IsPlayableTeam(capturingTeam)) {
        Logger::Warn("Ignoring Skirmish objective %u capture by invalid team %u",
                     objectiveId, capturingTeam);
        return;
    }
    auto* objectives = m_server ? m_server->GetObjectiveSystem() : nullptr;
    if (objectives) {
        const CaptureZone* captured = objectives->GetObjective(objectiveId);
        if (!captured || !captured->enabled || !captured->isActive ||
            captured->controllingTeam != capturingTeam) {
            Logger::Warn("Ignoring stale Skirmish objective %u capture event",
                         objectiveId);
            return;
        }
    }
    const auto previousCapture = m_lastObjectiveCaptureTeam.find(objectiveId);
    if (previousCapture != m_lastObjectiveCaptureTeam.end() &&
        previousCapture->second == capturingTeam) {
        return;
    }
    m_lastObjectiveCaptureTeam[objectiveId] = capturingTeam;

    Logger::Info("Skirmish objective %u captured by team %u", objectiveId, capturingTeam);
    ExtendSpawnWindow(capturingTeam);

    if (!objectives) return;

    bool contested = false;
    for (const CaptureZone* zone : objectives->GetAllObjectives()) {
        if (zone && zone->enabled && zone->captureProgress > 0.0f) {
            contested = true;
            break;
        }
    }
    const bool controlsAll = objectives->GetObjectiveCount() > 0 &&
                             objectives->AreAllObjectivesCapturedBy(capturingTeam);
    OnObjectiveControlChanged(capturingTeam, controlsAll, contested);
}

void SkirmishMode::OnObjectiveControlChanged(uint32_t controllingTeam,
                                              bool controlsAllObjectives,
                                              bool objectiveContested) {
    if (!CanCaptureObjectives()) return;

    m_objectiveContested = objectiveContested;

    if (!controlsAllObjectives || !IsPlayableTeam(controllingTeam)) {
        if (!m_suddenDeath || m_enteredSuddenDeathInOvertime) {
            CancelObjectiveLockdown();
        }
        return;
    }

    if (m_phase == Phase::InstantDeath) {
        FinishRoundWithWinner(controllingTeam);
        return;
    }
    if (m_phase != Phase::Active) return;

    // Repeated authoritative snapshots must not continually push the deadline.
    // A new all-objective capture event (or a different team taking all points)
    // starts one fresh 60-second lockdown.
    if (m_overtime && m_overtimeAdvantageTeam == controllingTeam) return;

    m_overtime = true;
    m_overtimeAdvantageTeam = controllingTeam;
    m_nextLockdownTime = std::max(
        GetRetailRemainingTime() - kAllObjectiveLockdownSeconds, 0);
    m_enteredSuddenDeathInOvertime = false;
    Logger::Info("Team %u controls every Skirmish objective; lockdown target=%d",
                 controllingTeam, m_nextLockdownTime);
}

void SkirmishMode::OnTicketsDepleted(uint32_t teamId) {
    if ((m_phase != Phase::Active && m_phase != Phase::InstantDeath) ||
        !IsPlayableTeam(teamId)) {
        return;
    }

    // TicketSystem's volley API commits both pools before publishing either
    // depletion transition. Resolve the double-zero snapshot as mutual
    // exhaustion; otherwise the deterministic first callback would award its
    // opposing team even though that side spent its final ticket in the same
    // combat transaction.
    if (m_server) {
        const TicketSystem* tickets = m_server->GetTicketSystem();
        if (tickets &&
            tickets->GetInitialTickets(kSouthTeamId) > 0 &&
            tickets->GetInitialTickets(kNorthTeamId) > 0 &&
            !tickets->HasTickets(kSouthTeamId) &&
            !tickets->HasTickets(kNorthTeamId)) {
            Logger::Info(
                "Skirmish round %d - mutual ticket exhaustion",
                m_currentRound);
            SetPhase(Phase::PostRound);
            return;
        }
    }

    const uint32_t winner = teamId == kSouthTeamId ? kNorthTeamId : kSouthTeamId;
    FinishRoundWithWinner(winner);
}

void SkirmishMode::OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {
    if (m_phase == Phase::Finished) return;
    RefreshAliveCounts();
    if (m_phase == Phase::Active) {
        CheckWinConditions();
    } else if (m_phase == Phase::InstantDeath) {
        CheckInstantDeathWinner();
    }
}

int SkirmishMode::GetTeamRoundWins(uint32_t teamId) const {
    if (teamId == kSouthTeamId) return m_team1Wins;
    if (teamId == kNorthTeamId) return m_team2Wins;
    return 0;
}

float SkirmishMode::GetRoundTimeRemaining() const {
    return m_phase == Phase::Active ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

float SkirmishMode::GetPhaseDuration() const {
    switch (m_phase) {
        case Phase::WarmUp:      return 0.0f;
        case Phase::Preparation: return m_preparationTime;
        case Phase::Active:      return m_roundTime;
        case Phase::InstantDeath:return m_instantDeathTime;
        case Phase::PostRound:   return m_postRoundTime;
        case Phase::NextRound:   return 5.0f;
        case Phase::Finished:    return 0.0f;
    }
    return 0.0f;
}

float SkirmishMode::GetPhaseTimeRemaining() const {
    return std::max(0.0f, m_phaseTimer);
}

float SkirmishMode::GetInstantDeathTimeRemaining() const {
    return m_phase == Phase::InstantDeath ? std::max(0.0f, m_phaseTimer) : 0.0f;
}

float SkirmishMode::GetLockdownTimeRemaining() const {
    if (!m_overtime || m_nextLockdownTime < 0) return 0.0f;
    return static_cast<float>(std::max(
        GetRetailRemainingTime() - m_nextLockdownTime, 0));
}

SkirmishMode::RetailState SkirmishMode::GetRetailState() const {
    RetailState state;
    state.allSpawnWindows.fill(-1);
    std::copy(kSpawnWindowOffsetsSeconds.begin(), kSpawnWindowOffsetsSeconds.end(),
              state.allSpawnWindows.begin());
    state.spawnWindowCloseTime = m_spawnWindowCloseTime;
    state.playedRoundsCount = m_playedRoundsCount;
    state.roundTeamScoreLimit = m_roundTeamScoreLimit;
    state.roundLimit = m_maxRounds;
    state.nextLockDownTime = m_nextLockdownTime;
    state.suddenDeath = m_suddenDeath;
    state.overTime = m_overtime;
    state.teamWithOvertimeAdvantage =
        TeamMapping::ServerToRetail(m_overtimeAdvantageTeam);
    state.playersAliveCount = m_playersAliveCount;
    return state;
}

std::array<int32_t, 20> SkirmishMode::GetAllSpawnWindows() const {
    std::array<int32_t, 20> windows;
    windows.fill(-1);
    std::copy(kSpawnWindowOffsetsSeconds.begin(), kSpawnWindowOffsetsSeconds.end(),
              windows.begin());
    return windows;
}

uint8_t SkirmishMode::GetPlayersAliveCount(uint32_t teamId) const {
    if (!IsPlayableTeam(teamId)) return 0;
    return m_playersAliveCount[RetailTeamSlot(teamId)];
}

bool SkirmishMode::IsSpawnWindowOpen(uint32_t teamId) const {
    if (m_phase != Phase::Active || m_suddenDeath || !IsPlayableTeam(teamId)) {
        return false;
    }
    return GetRetailRemainingTime() > m_spawnWindowCloseTime[RetailTeamSlot(teamId)];
}

bool SkirmishMode::CanReleaseSpawnWave(uint32_t teamId, float deltaSeconds) const {
    if (m_phase != Phase::Active || m_suddenDeath || !IsPlayableTeam(teamId)) {
        return false;
    }
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return false;

    const float elapsed = deltaSeconds;
    const int32_t postTickRemaining =
        ToRetailCountdown(std::max(0.0f, m_phaseTimer - elapsed));
    return postTickRemaining >= m_spawnWindowCloseTime[RetailTeamSlot(teamId)];
}

int32_t SkirmishMode::GetNextSpawnWaveTime(uint32_t teamId) const {
    if (!IsPlayableTeam(teamId)) return 0;
    const int32_t totalSpawnTimeLeft =
        GetRetailRemainingTime() - m_spawnWindowCloseTime[RetailTeamSlot(teamId)];
    for (const int32_t offset : kSpawnWindowOffsetsSeconds) {
        if (totalSpawnTimeLeft >= offset) {
            return std::max(totalSpawnTimeLeft - offset, 0);
        }
    }
    return 0;
}

int32_t SkirmishMode::GetSpawnWavesRemaining(uint32_t teamId) const {
    if (!IsSpawnWindowOpen(teamId)) return 0;
    const int32_t totalSpawnTimeLeft =
        GetRetailRemainingTime() - m_spawnWindowCloseTime[RetailTeamSlot(teamId)];
    if (totalSpawnTimeLeft < 1) return 0;
    return std::clamp(totalSpawnTimeLeft / kSpawnWaveIntervalSeconds + 1,
                      0, kSpawnWaveCount);
}

void SkirmishMode::SetMaxRounds(int rounds) {
    m_maxRounds = std::max(1, rounds);
}

void SkirmishMode::SetRoundTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) m_roundTime = seconds;
}

void SkirmishMode::SetInstantDeathTime(float seconds) {
    if (std::isfinite(seconds) && seconds >= 0.0f) {
        m_instantDeathTime = seconds;
    }
}

void SkirmishMode::SetTicketsPerRound(uint32_t tickets) {
    m_ticketsPerRound = tickets;
}

void SkirmishMode::SetPhase(Phase newPhase) {
    m_phase = newPhase;
    switch (newPhase) {
        case Phase::WarmUp:
            m_phaseTimer = 0.0f;
            break;
        case Phase::Preparation:
            m_phaseTimer = m_preparationTime;
            break;
        case Phase::Active:
            m_phaseTimer = m_roundTime;
            OpenInitialSpawnWindows();
            if (m_server) {
                if (auto* spawns = m_server->GetSpawnSystem()) {
                    // Initial deployment is the first h37 opportunity. Dead
                    // deployed players then release together at the remaining
                    // four 25-second marks, including the close boundary.
                    spawns->StartSpawnWave(kSouthTeamId);
                    spawns->StartSpawnWave(kNorthTeamId);
                }
            }
            break;
        case Phase::InstantDeath:
            m_phaseTimer = m_instantDeathTime;
            break;
        case Phase::PostRound:
            m_phaseTimer = m_postRoundTime;
            break;
        case Phase::NextRound:
            m_phaseTimer = 5.0f;
            break;
        case Phase::Finished:
            m_phaseTimer = 0.0f;
            break;
    }
    BroadcastPhaseChange();
}

void SkirmishMode::ResetRoundRetailState() {
    m_spawnWindowCloseTime = {0, 0};
    m_nextLockdownTime = -1;
    m_overtimeAdvantageTeam = 0;
    m_suddenDeath = false;
    m_overtime = false;
    m_objectiveContested = false;
    m_enteredSuddenDeathInOvertime = false;
    m_lastObjectiveCaptureTeam.clear();
}

void SkirmishMode::OpenInitialSpawnWindows() {
    const int32_t closeTime =
        ToRetailCountdown(m_roundTime) - kMaxSpawnWindowSeconds;
    m_spawnWindowCloseTime = {closeTime, closeTime};
}

void SkirmishMode::ExtendSpawnWindow(uint32_t teamId) {
    if (m_phase != Phase::Active || m_suddenDeath || !IsPlayableTeam(teamId)) return;

    const std::size_t slot = RetailTeamSlot(teamId);
    const int32_t remaining = GetRetailRemainingTime();
    constexpr int32_t captureBonus = kSpawnWaveIntervalSeconds - 1;
    const int32_t maximumExtensionTarget = remaining - kMaxSpawnWindowSeconds;

    if (m_spawnWindowCloseTime[slot] >= remaining) {
        m_spawnWindowCloseTime[slot] = std::max(remaining - captureBonus, 0);
    } else {
        m_spawnWindowCloseTime[slot] = std::max({
            m_spawnWindowCloseTime[slot] - captureBonus,
            maximumExtensionTarget,
            0});
    }

    if (m_server) {
        if (auto* spawns = m_server->GetSpawnSystem()) {
            // Capturing an objective moves this team's retail close-time
            // schedule. Re-arm the real deployment wave to the same next h37
            // mark instead of leaving it on the old round-relative cadence.
            spawns->SetWaveTimeRemaining(
                teamId, static_cast<float>(GetNextSpawnWaveTime(teamId)));
        }
    }
}

void SkirmishMode::RefreshAliveCounts() {
    m_playersAliveCount = {0, 0};
    auto* players = m_server ? m_server->GetPlayerManager() : nullptr;
    auto* teams = m_server ? m_server->GetTeamManager() : nullptr;
    auto* bots = m_server ? m_server->GetBotManager() : nullptr;

    const auto countAlive = [&](uint32_t teamId) {
        std::size_t alive = 0;
        if (players && teams) {
            for (const uint32_t playerId : teams->GetTeamPlayers(teamId)) {
                const auto player = players->GetPlayer(playerId);
                if (player && player->IsAlive()) ++alive;
            }
        }
        if (bots) {
            alive += bots->CountAliveBots(static_cast<uint8_t>(teamId));
        }
        return ToRetailAliveCount(alive);
    };

    m_playersAliveCount[TeamMapping::kRetailNva] = countAlive(kNorthTeamId);
    m_playersAliveCount[TeamMapping::kRetailUs] = countAlive(kSouthTeamId);
}

void SkirmishMode::RefreshObjectiveState() {
    auto* objectives = m_server ? m_server->GetObjectiveSystem() : nullptr;
    if (!objectives) return;
    if (objectives->GetObjectiveCount() == 0) {
        m_objectiveContested = false;
        CancelObjectiveLockdown();
        return;
    }

    m_objectiveContested = false;
    for (const CaptureZone* zone : objectives->GetAllObjectives()) {
        if (zone && zone->enabled && zone->isActive &&
            (zone->state == CaptureState::Contested ||
             zone->state == CaptureState::Capturing ||
             zone->captureProgress > 0.0f)) {
            m_objectiveContested = true;
            break;
        }
    }

    if (m_overtime &&
        !objectives->AreAllObjectivesCapturedBy(m_overtimeAdvantageTeam)) {
        if (!m_suddenDeath || m_enteredSuddenDeathInOvertime) {
            CancelObjectiveLockdown();
        }
    }
}

void SkirmishMode::BeginSuddenDeath() {
    if (m_suddenDeath) return;
    m_suddenDeath = true;
    m_spawnWindowCloseTime = {99999, 99999};
    SetPhase(Phase::InstantDeath);
    Logger::Info("Skirmish sudden death started; spawn windows closed");
}

void SkirmishMode::CancelObjectiveLockdown() {
    if (!m_overtime) return;
    m_overtime = false;
    m_nextLockdownTime = -1;
    m_overtimeAdvantageTeam = 0;
    m_enteredSuddenDeathInOvertime = false;
    Logger::Info("Skirmish all-objective lockdown cancelled");
}

bool SkirmishMode::CheckObjectiveLockdown() {
    if (!m_overtime || m_nextLockdownTime < 0 || m_objectiveContested ||
        !IsPlayableTeam(m_overtimeAdvantageTeam)) {
        return false;
    }
    if (GetRetailRemainingTime() > m_nextLockdownTime) return false;

    FinishRoundWithWinner(m_overtimeAdvantageTeam);
    return true;
}

void SkirmishMode::CheckWinConditions() {
    const bool southEliminated =
        GetPlayersAliveCount(kSouthTeamId) == 0 && !IsSpawnWindowOpen(kSouthTeamId);
    const bool northEliminated =
        GetPlayersAliveCount(kNorthTeamId) == 0 && !IsSpawnWindowOpen(kNorthTeamId);

    if (southEliminated && northEliminated) {
        Logger::Info("Skirmish round %d - mutual elimination", m_currentRound);
        SetPhase(Phase::PostRound);
    } else if (southEliminated) {
        FinishRoundWithWinner(kNorthTeamId);
    } else if (northEliminated) {
        FinishRoundWithWinner(kSouthTeamId);
    }
}

void SkirmishMode::CheckInstantDeathWinner() {
    const int southAlive = GetPlayersAliveCount(kSouthTeamId);
    const int northAlive = GetPlayersAliveCount(kNorthTeamId);

    if (southAlive == 0 && northAlive == 0) {
        Logger::Info("Skirmish round %d - sudden-death mutual elimination", m_currentRound);
        SetPhase(Phase::PostRound);
    } else if (southAlive == 0) {
        FinishRoundWithWinner(kNorthTeamId);
    } else if (northAlive == 0) {
        FinishRoundWithWinner(kSouthTeamId);
    } else if (m_phaseTimer <= 0.0f) {
        if (southAlive > northAlive) {
            FinishRoundWithWinner(kSouthTeamId);
        } else if (northAlive > southAlive) {
            FinishRoundWithWinner(kNorthTeamId);
        } else {
            Logger::Info("Skirmish round %d - sudden-death survivor tie", m_currentRound);
            SetPhase(Phase::PostRound);
        }
    }
}

void SkirmishMode::FinishRoundWithWinner(uint32_t teamId) {
    if (!IsPlayableTeam(teamId) || m_roundWinner != 0 ||
        (m_phase != Phase::Active && m_phase != Phase::InstantDeath)) {
        return;
    }
    AwardRoundWin(teamId);
    SetPhase(Phase::PostRound);
}

void SkirmishMode::AwardRoundWin(uint32_t teamId) {
    if (!IsPlayableTeam(teamId) || m_roundWinner != 0) return;
    m_roundWinner = teamId;
    if (teamId == kSouthTeamId) {
        ++m_team1Wins;
    } else {
        ++m_team2Wins;
    }
    Logger::Info("Team %u wins Skirmish round %d", teamId, m_currentRound);
    if (m_server) {
        m_server->BroadcastChatMessage("[Skirmish] Team " + std::to_string(teamId) +
                                       " wins round " +
                                       std::to_string(m_currentRound) + "!");
    }
}

void SkirmishMode::DetermineMatchWinner() {
    if (m_team1Wins > m_team2Wins) {
        Logger::Info("Team 1 wins Skirmish match (%d-%d)", m_team1Wins, m_team2Wins);
    } else if (m_team2Wins > m_team1Wins) {
        Logger::Info("Team 2 wins Skirmish match (%d-%d)", m_team2Wins, m_team1Wins);
    } else {
        Logger::Info("Skirmish match tied (%d-%d)", m_team1Wins, m_team2Wins);
    }
}

void SkirmishMode::BroadcastPhaseChange() const {
    if (!m_server) return;

    std::string msg;
    switch (m_phase) {
        case Phase::WarmUp:
            msg = "Waiting for players...";
            break;
        case Phase::Preparation:
            msg = "Round " + std::to_string(m_currentRound) + " starting!";
            break;
        case Phase::Active:
            msg = "Round " + std::to_string(m_currentRound) + " active!";
            break;
        case Phase::InstantDeath:
            msg = "SUDDEN DEATH - No respawns!";
            break;
        case Phase::PostRound:
            msg = "Round over!";
            break;
        case Phase::NextRound:
            msg = "Next round starting...";
            break;
        case Phase::Finished:
            msg = "Match complete!";
            break;
    }
    m_server->BroadcastChatMessage("[Skirmish] " + msg);
}

int32_t SkirmishMode::GetRetailRemainingTime() const {
    return m_phase == Phase::Active ? ToRetailCountdown(m_phaseTimer) : 0;
}

bool SkirmishMode::IsPlayableTeam(uint32_t teamId) {
    return teamId == kSouthTeamId || teamId == kNorthTeamId;
}

std::size_t SkirmishMode::RetailTeamSlot(uint32_t teamId) {
    return static_cast<std::size_t>(TeamMapping::ServerToRetail(teamId));
}
