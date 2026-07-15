#include "TestFramework.h"

#include "Game/TicketSystem.h"

#include <limits>
#include <vector>

namespace {

TEST(TicketSystem, InitialZeroPoolRemainsUnlimited) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(0, 4);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted(
        [&](uint32_t) { ++callbackCount; });

    tickets.ConsumeTicket(1);
    tickets.ConsumeTicket(1, std::numeric_limits<uint32_t>::max());
    tickets.SetTickets(1, 3);
    tickets.SetTickets(1, 0);
    tickets.SetTickets(1, 2);
    tickets.Reset();

    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_TRUE(tickets.HasTickets(1));
    EXPECT_EQ(callbackCount, 0u);
    EXPECT_FALSE(tickets.HasTickets(999u));
}

TEST(TicketSystem, PositiveToZeroFiresOnceAndRepeatedDeathsAtZeroDoNotReenter) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted([&](uint32_t teamId) {
        ++callbackCount;
        // Synchronous callbacks are permitted to query/mutate the system. A
        // redundant death at zero must not recursively invoke this callback.
        tickets.OnPlayerKilled(teamId);
    });

    tickets.OnPlayerKilled(1);
    tickets.OnPlayerKilled(1);
    tickets.ConsumeTicket(1, 0);

    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(callbackCount, 1u);
}

TEST(TicketSystem, DeathBatchCommitsEveryPoolBeforeFirstDepletionCallback) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);

    std::vector<uint32_t> depletedTeams;
    bool callbackObservedPartialBatch = false;
    tickets.SetOnTicketsDepleted([&](uint32_t teamId) {
        depletedTeams.push_back(teamId);
        callbackObservedPartialBatch = callbackObservedPartialBatch ||
            tickets.GetTickets(1) != 0u || tickets.GetTickets(2) != 0u;
    });

    // Publish North first to prove the transaction is independent of the
    // deterministic victim-id ordering used by BotManager.
    tickets.OnPlayersKilled({2u, 1u});

    ASSERT_EQ(depletedTeams.size(), 2u);
    EXPECT_EQ(depletedTeams[0], 2u);
    EXPECT_EQ(depletedTeams[1], 1u);
    EXPECT_FALSE(callbackObservedPartialBatch);
    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(tickets.GetTickets(2), 0u);
}

TEST(TicketSystem, SingletonDeathBatchPreservesSynchronousDeathSemantics) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted([&](uint32_t teamId) {
        ++callbackCount;
        EXPECT_EQ(teamId, 1u);
        EXPECT_EQ(tickets.GetTickets(1), 0u);
        EXPECT_EQ(tickets.GetTickets(2), 1u);
    });

    tickets.OnPlayersKilled({1u});

    EXPECT_EQ(callbackCount, 1u);
}

TEST(TicketSystem, DeathBatchSnapshotsObserverAcrossCallbackReplacement) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);

    std::vector<uint32_t> originalObserver;
    std::vector<uint32_t> replacementObserver;
    tickets.SetOnTicketsDepleted([&](uint32_t teamId) {
        originalObserver.push_back(teamId);
        tickets.SetOnTicketsDepleted([&](uint32_t replacementTeamId) {
            replacementObserver.push_back(replacementTeamId);
        });
    });

    tickets.OnPlayersKilled({1u, 2u});

    ASSERT_EQ(originalObserver.size(), 2u);
    EXPECT_EQ(originalObserver[0], 1u);
    EXPECT_EQ(originalObserver[1], 2u);
    EXPECT_TRUE(replacementObserver.empty());
}

TEST(TicketSystem, ReplenishmentAllowsASecondDepletionTransition) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(2, 2);

    std::vector<uint32_t> depletedTeams;
    tickets.SetOnTicketsDepleted(
        [&](uint32_t teamId) { depletedTeams.push_back(teamId); });

    tickets.ConsumeTicket(1, 2);
    tickets.SetTickets(1, 3);
    tickets.SetTickets(1, 0);
    tickets.SetTickets(1, 0);

    ASSERT_EQ(depletedTeams.size(), 2u);
    EXPECT_EQ(depletedTeams[0], 1u);
    EXPECT_EQ(depletedTeams[1], 1u);
}

TEST(TicketSystem, SynchronousReplenishmentCreatesANewTransition) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted([&](uint32_t teamId) {
        ++callbackCount;
        if (callbackCount == 1) {
            tickets.AddTickets(teamId, 1);
            tickets.ConsumeTicket(teamId);
        }
    });

    tickets.ConsumeTicket(1);

    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(callbackCount, 2u);
}

TEST(TicketSystem, ResetReplenishesFinitePoolAndRearmsDepletion) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(2, 4);

    std::vector<uint32_t> depletedTeams;
    tickets.SetOnTicketsDepleted(
        [&](uint32_t teamId) { depletedTeams.push_back(teamId); });

    tickets.ConsumeTicket(1, 2);
    tickets.Reset();
    EXPECT_EQ(tickets.GetTickets(1), 2u);
    EXPECT_EQ(depletedTeams.size(), 1u);

    tickets.ConsumeTicket(1, std::numeric_limits<uint32_t>::max());

    ASSERT_EQ(depletedTeams.size(), 2u);
    EXPECT_EQ(depletedTeams[0], 1u);
    EXPECT_EQ(depletedTeams[1], 1u);
}

TEST(TicketSystem, ConsumptionClampsAtZeroAndAdditionSaturates) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(3, 3);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted(
        [&](uint32_t) { ++callbackCount; });

    tickets.ConsumeTicket(1, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(callbackCount, 1u);

    tickets.AddTickets(1, std::numeric_limits<uint32_t>::max());
    tickets.AddTickets(1, 1);
    EXPECT_EQ(tickets.GetTickets(1), std::numeric_limits<uint32_t>::max());

    tickets.ConsumeTicket(1, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(callbackCount, 2u);
}

TEST(TicketSystem, InvalidBleedInputsCannotPoisonOrLoopTheAccumulator) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(3, 3);
    tickets.EnableBleed(true);

    tickets.SetBleedRate(1, std::numeric_limits<float>::quiet_NaN());
    tickets.Update(std::numeric_limits<float>::infinity());
    tickets.Update(-1.0f);
    EXPECT_EQ(tickets.GetTickets(1), 3u);
    EXPECT_FLOAT_EQ(tickets.GetBleedRate(1), 0.0f);

    tickets.SetBleedRate(1, std::numeric_limits<float>::max());
    tickets.Update(std::numeric_limits<float>::max());
    EXPECT_EQ(tickets.GetTickets(1), 0u);
}

TEST(TicketSystem, FractionalAndMultiTicketBleedCarryDeterministically) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(5, 5);
    tickets.EnableBleed(true);
    tickets.SetBleedRate(1, 2.5f);

    uint32_t callbackCount = 0;
    tickets.SetOnTicketsDepleted(
        [&](uint32_t teamId) {
            EXPECT_EQ(teamId, 1u);
            ++callbackCount;
        });

    tickets.Update(1.0f);  // two tickets consumed, 0.5 carried
    EXPECT_EQ(tickets.GetTickets(1), 3u);
    EXPECT_EQ(callbackCount, 0u);

    tickets.Update(0.2f);  // carried 0.5 + new 0.5 consumes exactly one
    EXPECT_EQ(tickets.GetTickets(1), 2u);

    tickets.Update(0.2f);  // new 0.5 remains fractional
    EXPECT_EQ(tickets.GetTickets(1), 2u);
    tickets.Update(0.2f);  // next 0.5 completes the ticket
    EXPECT_EQ(tickets.GetTickets(1), 1u);

    tickets.Update(10.0f);
    EXPECT_EQ(tickets.GetTickets(1), 0u);
    EXPECT_EQ(callbackCount, 1u);
    tickets.Update(10.0f);
    EXPECT_EQ(callbackCount, 1u);
}

TEST(TicketSystem, DepletionClearsFractionalBleedBeforeReplenishment) {
    TicketSystem tickets(nullptr);
    tickets.Initialize(1, 1);
    tickets.EnableBleed(true);
    tickets.SetBleedRate(1, 1.5f);

    tickets.Update(1.0f);
    ASSERT_EQ(tickets.GetTickets(1), 0u);
    tickets.SetTickets(1, 2u);

    // Depletion discards the old 0.5 remainder. A new half-second therefore
    // accrues only 0.75 and cannot consume a replenished ticket yet.
    tickets.Update(0.5f);
    EXPECT_EQ(tickets.GetTickets(1), 2u);
    tickets.Update(0.5f);
    EXPECT_EQ(tickets.GetTickets(1), 1u);
}

} // namespace

RS2V_TEST_MAIN()
