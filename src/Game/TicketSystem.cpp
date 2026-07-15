// src/Game/TicketSystem.cpp
// RS2V reinforcement ticket system implementation

#include "Game/TicketSystem.h"
#include "Game/GameServer.h"
#include "Utils/Logger.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

TicketSystem::TicketSystem(GameServer* server)
    : m_server(server)
{
}

TicketSystem::~TicketSystem() = default;

void TicketSystem::Initialize(uint32_t team1Tickets, uint32_t team2Tickets) {
    m_teamTickets[1] = {team1Tickets, team1Tickets, 0.0f, 0.0f, 1};
    m_teamTickets[2] = {team2Tickets, team2Tickets, 0.0f, 0.0f, 1};
    m_bleedEnabled = false;
    Logger::Info("TicketSystem initialized: Team1=%u, Team2=%u", team1Tickets, team2Tickets);
}

void TicketSystem::Reset() {
    std::vector<uint32_t> depletedTeams;
    for (auto& [teamId, state] : m_teamTickets) {
        const uint32_t previousTickets = state.current;
        state.current = state.initial;
        state.bleedRate = 0.0f;
        state.bleedAccumulator = 0.0f;
        if (IsDepletionTransition(state, previousTickets)) {
            depletedTeams.push_back(teamId);
        }
    }

    // Finish resetting every team before invoking user code. A callback can
    // safely inspect or mutate either pool without observing a half-reset map.
    for (const uint32_t teamId : depletedTeams) {
        NotifyTicketsDepleted(teamId);
    }
    Logger::Info("TicketSystem reset");
}

void TicketSystem::ConsumeTicket(uint32_t teamId, uint32_t count) {
    if (ApplyTicketConsumption(teamId, count)) {
        NotifyTicketsDepleted(teamId);
    }
}

bool TicketSystem::ApplyTicketConsumption(uint32_t teamId, uint32_t count) {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end() || count == 0) return false;

    auto& state = it->second;
    // A configured pool of zero is the existing unlimited-ticket sentinel.
    // Its displayed count remains zero and must never emit depletion events.
    if (state.initial == 0) return false;

    const uint32_t previousTickets = state.current;
    state.current = count >= state.current ? 0 : state.current - count;

    Logger::Debug("Team %u consumed %u ticket(s), remaining: %u",
                  teamId, count, state.current);
    return IsDepletionTransition(state, previousTickets);
}

void TicketSystem::SetTickets(uint32_t teamId, uint32_t count) {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end()) return;

    auto& state = it->second;
    const uint32_t previousTickets = state.current;
    state.current = count;
    Logger::Debug("Team %u ticket count set to %u", teamId, count);
    if (IsDepletionTransition(state, previousTickets)) {
        NotifyTicketsDepleted(teamId);
    }
}

void TicketSystem::AddTickets(uint32_t teamId, uint32_t count) {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end() || count == 0) return;

    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    it->second.current = count > maximum - it->second.current
        ? maximum
        : it->second.current + count;
    Logger::Debug("Team %u gained %u ticket(s), total: %u", teamId, count, it->second.current);
}

uint32_t TicketSystem::GetTickets(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() ? it->second.current : 0;
}

bool TicketSystem::HasTickets(uint32_t teamId) const {
    const auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() &&
           (it->second.initial == 0 || it->second.current > 0);
}

void TicketSystem::SetBleedRate(uint32_t teamId, float ticketsPerSecond) {
    auto it = m_teamTickets.find(teamId);
    if (it != m_teamTickets.end()) {
        it->second.bleedRate = std::isfinite(ticketsPerSecond) && ticketsPerSecond > 0.0f
            ? ticketsPerSecond
            : 0.0f;
    }
}

float TicketSystem::GetBleedRate(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() ? it->second.bleedRate : 0.0f;
}

void TicketSystem::EnableBleed(bool enable) {
    m_bleedEnabled = enable;
}

void TicketSystem::Update(float deltaSeconds) {
    if (!m_bleedEnabled || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;

    for (auto& [teamId, state] : m_teamTickets) {
        if (state.initial == 0 || state.bleedRate <= 0.0f || state.current == 0) continue;

        const double accrued = static_cast<double>(state.bleedAccumulator) +
                               static_cast<double>(state.bleedRate) * deltaSeconds;
        const double wholeTickets = std::floor(accrued);
        if (wholeTickets < 1.0) {
            state.bleedAccumulator = static_cast<float>(accrued);
            continue;
        }

        const uint32_t ticketsToConsume = wholeTickets >= state.current
            ? state.current
            : static_cast<uint32_t>(wholeTickets);
        state.bleedAccumulator = ticketsToConsume == state.current
            ? 0.0f
            : static_cast<float>(accrued - wholeTickets);
        ConsumeTicket(teamId, ticketsToConsume);
    }
}

void TicketSystem::SetOnTicketsDepleted(TicketDepletedCallback cb) {
    m_depletedCallback = std::move(cb);
}

void TicketSystem::OnPlayerKilled(uint32_t victimTeamId) {
    auto it = m_teamTickets.find(victimTeamId);
    if (it == m_teamTickets.end()) return;
    ConsumeTicket(victimTeamId, it->second.ticketLossPerDeath);
}

void TicketSystem::OnPlayersKilled(
    const std::vector<uint32_t>& victimTeamIds) {
    std::vector<uint32_t> depletedTeams;
    depletedTeams.reserve(victimTeamIds.size());

    // Do not route this loop through OnPlayerKilled/ConsumeTicket: their
    // ordinary single-event API intentionally publishes synchronously. The
    // batch transaction first exposes every debit, then publishes transitions.
    for (const uint32_t teamId : victimTeamIds) {
        const auto it = m_teamTickets.find(teamId);
        if (it == m_teamTickets.end()) continue;
        if (ApplyTicketConsumption(teamId,
                                   it->second.ticketLossPerDeath)) {
            depletedTeams.push_back(teamId);
        }
    }

    NotifyTicketsDepletedBatch(depletedTeams);
}

void TicketSystem::OnObjectiveLost(uint32_t teamId, uint32_t ticketPenalty) {
    ConsumeTicket(teamId, ticketPenalty);
    Logger::Info("Team %u lost %u tickets from objective loss", teamId, ticketPenalty);
}

void TicketSystem::SetTicketLossPerDeath(uint32_t teamId, uint32_t loss) {
    auto it = m_teamTickets.find(teamId);
    if (it != m_teamTickets.end()) {
        it->second.ticketLossPerDeath = loss;
    }
}

uint32_t TicketSystem::GetTicketLossPerDeath(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() ? it->second.ticketLossPerDeath : 1;
}

uint32_t TicketSystem::GetInitialTickets(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() ? it->second.initial : 0;
}

float TicketSystem::GetTicketPercentage(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end() || it->second.initial == 0) return 0.0f;
    return static_cast<float>(it->second.current) / static_cast<float>(it->second.initial) * 100.0f;
}

bool TicketSystem::IsDepletionTransition(const TeamTicketState& state,
                                         uint32_t previousTickets) noexcept {
    // initial == 0 is the established unlimited-ticket sentinel. Its visible
    // count may be set for diagnostics, but it never becomes depleted.
    return state.initial > 0 && previousTickets > 0 && state.current == 0;
}

void TicketSystem::NotifyTicketsDepleted(uint32_t teamId) {
    if (!m_depletedCallback) return;

    // Copy before invocation so a callback may replace/clear the registered
    // function without invalidating the callable currently on the stack.
    const TicketDepletedCallback callback = m_depletedCallback;
    Logger::Info("Team %u tickets depleted!", teamId);
    callback(teamId);
}

void TicketSystem::NotifyTicketsDepletedBatch(
    const std::vector<uint32_t>& teamIds) {
    if (teamIds.empty() || !m_depletedCallback) return;

    // One stable observer receives the complete transaction even if its first
    // invocation replaces the registered callback. Reentrant ticket changes
    // remain legal and use the newly registered callback independently.
    const TicketDepletedCallback callback = m_depletedCallback;
    for (const uint32_t teamId : teamIds) {
        Logger::Info("Team %u tickets depleted!", teamId);
        callback(teamId);
    }
}
