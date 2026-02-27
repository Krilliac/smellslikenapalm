// src/Game/TicketSystem.cpp
// RS2V reinforcement ticket system implementation

#include "Game/TicketSystem.h"
#include "Game/GameServer.h"
#include "Utils/Logger.h"

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
    for (auto& [teamId, state] : m_teamTickets) {
        state.current = state.initial;
        state.bleedRate = 0.0f;
        state.bleedAccumulator = 0.0f;
    }
    Logger::Info("TicketSystem reset");
}

void TicketSystem::ConsumeTicket(uint32_t teamId, uint32_t count) {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end()) return;

    if (it->second.current >= count) {
        it->second.current -= count;
    } else {
        it->second.current = 0;
    }

    Logger::Debug("Team %u consumed %u ticket(s), remaining: %u", teamId, count, it->second.current);
    CheckDepletion(teamId);
}

void TicketSystem::AddTickets(uint32_t teamId, uint32_t count) {
    auto it = m_teamTickets.find(teamId);
    if (it == m_teamTickets.end()) return;
    it->second.current += count;
    Logger::Debug("Team %u gained %u ticket(s), total: %u", teamId, count, it->second.current);
}

uint32_t TicketSystem::GetTickets(uint32_t teamId) const {
    auto it = m_teamTickets.find(teamId);
    return it != m_teamTickets.end() ? it->second.current : 0;
}

bool TicketSystem::HasTickets(uint32_t teamId) const {
    return GetTickets(teamId) > 0;
}

void TicketSystem::SetBleedRate(uint32_t teamId, float ticketsPerSecond) {
    auto it = m_teamTickets.find(teamId);
    if (it != m_teamTickets.end()) {
        it->second.bleedRate = ticketsPerSecond;
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
    if (!m_bleedEnabled) return;

    for (auto& [teamId, state] : m_teamTickets) {
        if (state.bleedRate <= 0.0f || state.current == 0) continue;

        state.bleedAccumulator += state.bleedRate * deltaSeconds;
        while (state.bleedAccumulator >= 1.0f) {
            state.bleedAccumulator -= 1.0f;
            if (state.current > 0) {
                state.current--;
                Logger::Debug("Team %u lost ticket from bleed, remaining: %u", teamId, state.current);
                CheckDepletion(teamId);
                if (state.current == 0) break;
            }
        }
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

void TicketSystem::CheckDepletion(uint32_t teamId) {
    if (GetTickets(teamId) == 0 && m_depletedCallback) {
        Logger::Info("Team %u tickets depleted!", teamId);
        m_depletedCallback(teamId);
    }
}
