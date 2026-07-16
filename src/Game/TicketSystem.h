// src/Game/TicketSystem.h
// RS2V reinforcement ticket system — tracks team reinforcements and bleed-out

#pragma once

#include <cstdint>
#include <map>
#include <chrono>
#include <functional>
#include <vector>

class GameServer;

// Callback when a team runs out of tickets
using TicketDepletedCallback = std::function<void(uint32_t teamId)>;

class TicketSystem {
public:
    explicit TicketSystem(GameServer* server);
    ~TicketSystem();

    void Initialize(uint32_t team1Tickets, uint32_t team2Tickets);
    void Reset();

    // Core ticket operations
    void ConsumeTicket(uint32_t teamId, uint32_t count = 1);
    // Sets the current pool without changing its configured/reset value or
    // the initial-zero unlimited designation.
    void SetTickets(uint32_t teamId, uint32_t count);
    void AddTickets(uint32_t teamId, uint32_t count);
    uint32_t GetTickets(uint32_t teamId) const;
    bool HasTickets(uint32_t teamId) const;

    // Ticket bleed: teams lose tickets over time when enemy holds more objectives
    void SetBleedRate(uint32_t teamId, float ticketsPerSecond);
    float GetBleedRate(uint32_t teamId) const;
    void EnableBleed(bool enable);

    // Per-tick update (processes bleed)
    void Update(float deltaSeconds);

    // Events
    void SetOnTicketsDepleted(TicketDepletedCallback cb);
    void OnPlayerKilled(uint32_t victimTeamId);
    // Applies one simultaneous death transaction. Every valid ticket debit is
    // committed before the first depletion callback runs, so callback code can
    // inspect both pools without observing an order-dependent partial volley.
    // A one-element batch has the same observable ordering as OnPlayerKilled.
    void OnPlayersKilled(const std::vector<uint32_t>& victimTeamIds);
    void OnObjectiveLost(uint32_t teamId, uint32_t ticketPenalty);

    // Configuration
    void SetTicketLossPerDeath(uint32_t teamId, uint32_t loss);
    uint32_t GetTicketLossPerDeath(uint32_t teamId) const;

    // Query
    uint32_t GetInitialTickets(uint32_t teamId) const;
    float GetTicketPercentage(uint32_t teamId) const;

private:
    GameServer* m_server;

    struct TeamTicketState {
        uint32_t current = 0;
        uint32_t initial = 0;
        float bleedRate = 0.0f;           // tickets lost per second
        float bleedAccumulator = 0.0f;    // fractional bleed accumulation
        uint32_t ticketLossPerDeath = 1;
    };

    std::map<uint32_t, TeamTicketState> m_teamTickets;
    bool m_bleedEnabled = false;
    TicketDepletedCallback m_depletedCallback;

    [[nodiscard]] static bool IsDepletionTransition(
        const TeamTicketState& state, uint32_t previousTickets) noexcept;
    [[nodiscard]] bool ApplyTicketConsumption(uint32_t teamId,
                                              uint32_t count);
    void NotifyTicketsDepleted(uint32_t teamId);
    void NotifyTicketsDepletedBatch(
        const std::vector<uint32_t>& teamIds);
};
