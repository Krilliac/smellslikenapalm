// tests/DeploymentCoordinatorTests.cpp

#include "TestFramework.h"

#include "Game/DeploymentCoordinator.h"

#include <array>
#include <limits>
#include <span>
#include <vector>

namespace {

using Coordinator = DeploymentCoordinator;
using Countdown = DeploymentCountdown;

TEST(DeploymentCoordinator, PublishesSemanticHandlesWithoutWireAssumptions) {
    static_assert(Coordinator::kServerSetSpawnSelectHandle == 261u);
    static_assert(Coordinator::kServerSetReadyToSpawnHandle == 434u);
    static_assert(Coordinator::kNormalSpawnSelectionBase == 128u);
    static_assert(Coordinator::kNormalSpawnSlotCount == 10u);
    static_assert(Coordinator::kFirstSpecialSelection == 237u);
    static_assert(Coordinator::kHelicopterSelection == 252u);
    static_assert(Coordinator::kTunnelSelection == 253u);
    static_assert(Coordinator::kSquadLeaderSelection == 254u);

    EXPECT_EQ(static_cast<uint8_t>(Coordinator::ReadyStatus::Ready), 0u);
    EXPECT_EQ(static_cast<uint8_t>(Coordinator::ReadyStatus::ForceOnly), 1u);
    EXPECT_EQ(static_cast<uint8_t>(Coordinator::ReadyStatus::NotReady), 2u);
}

TEST(DeploymentCoordinator, EnforcesRoleSelectionReadyOrderAndAuthorizesOnce) {
    Coordinator coordinator(7u);
    const std::array<uint32_t, 2> available = {410u, 420u};

    EXPECT_EQ(coordinator.SelectSpawn(12u, 129u, available),
              Coordinator::SelectionResult::RoleNotFinalized);
    const auto earlyReady = coordinator.SetReadyStatus(
        12u, Coordinator::ReadyStatus::Ready, available);
    EXPECT_EQ(earlyReady.result, Coordinator::ReadyResult::RoleNotFinalized);
    EXPECT_FALSE(earlyReady.spawnId.has_value());

    coordinator.FinalizeRole(12u);
    const auto roleState = coordinator.GetClientState(12u);
    ASSERT_TRUE(roleState.has_value());
    EXPECT_TRUE(roleState->roleFinalized);
    EXPECT_EQ(roleState->readyStatus, Coordinator::ReadyStatus::ForceOnly);

    EXPECT_EQ(coordinator.SelectSpawn(12u, 129u, available),
              Coordinator::SelectionResult::Accepted);
    const auto selectedState = coordinator.GetClientState(12u);
    ASSERT_TRUE(selectedState.has_value());
    ASSERT_TRUE(selectedState->selectedSlot.has_value());
    ASSERT_TRUE(selectedState->selectedSpawnId.has_value());
    EXPECT_EQ(*selectedState->selectedSlot, 1u);
    EXPECT_EQ(*selectedState->selectedSpawnId, 420u);
    EXPECT_EQ(selectedState->readyStatus, Coordinator::ReadyStatus::ForceOnly);

    const auto forceOnly = coordinator.SetReadyStatus(
        12u, Coordinator::ReadyStatus::ForceOnly, available);
    EXPECT_EQ(forceOnly.result, Coordinator::ReadyResult::StatusRecorded);
    EXPECT_FALSE(forceOnly.spawnId.has_value());

    const auto authorized = coordinator.SetReadyStatus(
        12u, Coordinator::ReadyStatus::Ready, available);
    EXPECT_TRUE(authorized.IsNewAuthorization());
    ASSERT_TRUE(authorized.spawnId.has_value());
    EXPECT_EQ(*authorized.spawnId, 420u);
    EXPECT_TRUE(coordinator.IsPreparedForDeployment(12u));

    const auto duplicateReady = coordinator.SetReadyStatus(
        12u, Coordinator::ReadyStatus::Ready, available);
    EXPECT_EQ(duplicateReady.result, Coordinator::ReadyResult::AlreadyAuthorized);
    EXPECT_FALSE(duplicateReady.spawnId.has_value());
    EXPECT_EQ(coordinator.SelectSpawn(12u, 128u, available),
              Coordinator::SelectionResult::AlreadyAuthorized);
}

TEST(DeploymentCoordinator, RejectsSpecialAndOutOfRangeSelectionsFailClosed) {
    Coordinator coordinator;
    coordinator.FinalizeRole(1u);
    const std::array<uint32_t, 2> available = {10u, 20u};

    for (const uint8_t special : {
             Coordinator::kFirstSpecialSelection,
             static_cast<uint8_t>(251u),
             Coordinator::kHelicopterSelection,
             Coordinator::kTunnelSelection,
             Coordinator::kSquadLeaderSelection}) {
        EXPECT_EQ(coordinator.SelectSpawn(1u, special, available),
                  Coordinator::SelectionResult::UnsupportedSpecialSelection);
        const auto state = coordinator.GetClientState(1u);
        ASSERT_TRUE(state.has_value());
        EXPECT_FALSE(state->selectedSlot.has_value());
        EXPECT_FALSE(state->selectedSpawnId.has_value());
    }

    EXPECT_EQ(coordinator.SelectSpawn(1u, 127u, available),
              Coordinator::SelectionResult::InvalidNormalSelection);
    EXPECT_EQ(coordinator.SelectSpawn(1u, 138u, available),
              Coordinator::SelectionResult::InvalidNormalSelection);
    EXPECT_EQ(coordinator.SelectSpawn(1u, 130u, available),
              Coordinator::SelectionResult::SlotUnavailable);

    const std::array<uint32_t, 10> allNormalSlots = {
        100u, 101u, 102u, 103u, 104u,
        105u, 106u, 107u, 108u, 109u
    };
    EXPECT_EQ(coordinator.SelectSpawn(1u, 137u, allNormalSlots),
              Coordinator::SelectionResult::Accepted);
    const auto state = coordinator.GetClientState(1u);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->selectedSlot, std::optional<uint8_t>{9u});
    EXPECT_EQ(state->selectedSpawnId, std::optional<uint32_t>{109u});
}

TEST(DeploymentCoordinator, RequiresFreshReadyAndExactAuthoritativeSlotMapping) {
    Coordinator coordinator;
    coordinator.FinalizeRole(4u);
    const std::array<uint32_t, 2> firstList = {50u, 60u};
    const std::array<uint32_t, 2> changedList = {50u, 70u};

    // Ready without a selection records no authorization. An accepted h261
    // resets the client to ForceOnly, requiring the subsequent Ready edge.
    const auto noSelection = coordinator.SetReadyStatus(
        4u, Coordinator::ReadyStatus::Ready, firstList);
    EXPECT_EQ(noSelection.result, Coordinator::ReadyResult::NoSelection);
    EXPECT_EQ(coordinator.SelectSpawn(4u, 129u, firstList),
              Coordinator::SelectionResult::Accepted);
    const auto afterSelection = coordinator.GetClientState(4u);
    ASSERT_TRUE(afterSelection.has_value());
    EXPECT_EQ(afterSelection->readyStatus, Coordinator::ReadyStatus::ForceOnly);

    // The selected slot changed between h261 and h434. Never reinterpret it as
    // the new spawn; clear it and require a new client selection.
    const auto stale = coordinator.SetReadyStatus(
        4u, Coordinator::ReadyStatus::Ready, changedList);
    EXPECT_EQ(stale.result,
              Coordinator::ReadyResult::SelectionNoLongerAvailable);
    EXPECT_FALSE(stale.spawnId.has_value());
    const auto staleState = coordinator.GetClientState(4u);
    ASSERT_TRUE(staleState.has_value());
    EXPECT_FALSE(staleState->selectedSlot.has_value());
    EXPECT_FALSE(staleState->selectedSpawnId.has_value());
    EXPECT_EQ(staleState->readyStatus, Coordinator::ReadyStatus::ForceOnly);

    EXPECT_EQ(coordinator.SelectSpawn(4u, 129u, changedList),
              Coordinator::SelectionResult::Accepted);
    const auto fresh = coordinator.SetReadyStatus(
        4u, Coordinator::ReadyStatus::Ready, changedList);
    EXPECT_TRUE(fresh.IsNewAuthorization());
    EXPECT_EQ(fresh.spawnId, std::optional<uint32_t>{70u});
}

TEST(DeploymentCoordinator, NotReadyNeverAuthorizesAndPreparedOrderIsDeterministic) {
    Coordinator coordinator(3u);
    const std::array<uint32_t, 1> available = {900u};

    for (const uint32_t clientId : {9u, 2u}) {
        coordinator.FinalizeRole(clientId);
        ASSERT_EQ(coordinator.SelectSpawn(clientId, 128u, available),
                  Coordinator::SelectionResult::Accepted);
    }

    const auto notReady = coordinator.SetReadyStatus(
        9u, Coordinator::ReadyStatus::NotReady, available);
    EXPECT_EQ(notReady.result, Coordinator::ReadyResult::StatusRecorded);
    EXPECT_FALSE(coordinator.IsPreparedForDeployment(9u));

    EXPECT_TRUE(coordinator.SetReadyStatus(
        9u, Coordinator::ReadyStatus::Ready, available).IsNewAuthorization());
    EXPECT_TRUE(coordinator.SetReadyStatus(
        2u, Coordinator::ReadyStatus::Ready, available).IsNewAuthorization());

    // Reopening the scene (ForceOnly/NotReady) revokes an authorization that
    // has not yet reached the countdown deployment threshold. A subsequent
    // Ready is a new edge, while duplicate Ready above remained idempotent.
    const auto revoked = coordinator.SetReadyStatus(
        9u, Coordinator::ReadyStatus::ForceOnly, available);
    EXPECT_EQ(revoked.result, Coordinator::ReadyResult::StatusRecorded);
    EXPECT_FALSE(coordinator.IsPreparedForDeployment(9u));
    EXPECT_TRUE(coordinator.SetReadyStatus(
        9u, Coordinator::ReadyStatus::Ready, available).IsNewAuthorization());

    const auto prepared = coordinator.GetPreparedDeployments();
    ASSERT_EQ(prepared.size(), 2u);
    EXPECT_EQ(prepared[0], std::make_pair(2u, 900u));
    EXPECT_EQ(prepared[1], std::make_pair(9u, 900u));
}

TEST(DeploymentCoordinator, ClientAndGenerationResetRemoveEveryAuthorization) {
    Coordinator coordinator(11u);
    const std::array<uint32_t, 1> available = {77u};

    coordinator.FinalizeRole(5u);
    ASSERT_EQ(coordinator.SelectSpawn(5u, 128u, available),
              Coordinator::SelectionResult::Accepted);
    ASSERT_TRUE(coordinator.SetReadyStatus(
        5u, Coordinator::ReadyStatus::Ready, available).IsNewAuthorization());

    coordinator.ResetClient(5u);
    const auto resetClient = coordinator.GetClientState(5u);
    ASSERT_TRUE(resetClient.has_value());
    EXPECT_EQ(resetClient->generation, 11u);
    EXPECT_FALSE(resetClient->roleFinalized);
    EXPECT_FALSE(resetClient->deploymentAuthorized);

    coordinator.RemoveClient(5u);
    EXPECT_FALSE(coordinator.GetClientState(5u).has_value());

    coordinator.FinalizeRole(6u);
    coordinator.ResetForGeneration(12u);
    EXPECT_EQ(coordinator.GetGeneration(), 12u);
    EXPECT_FALSE(coordinator.GetClientState(6u).has_value());
    EXPECT_TRUE(coordinator.GetPreparedDeployments().empty());

    // Reset is explicit even when a caller reuses its numeric generation token.
    coordinator.FinalizeRole(7u);
    coordinator.ResetForGeneration(12u);
    EXPECT_FALSE(coordinator.GetClientState(7u).has_value());
}

TEST(DeploymentCountdown, UsesThirtySecondPreparationAndFinalEightSecondWindow) {
    static_assert(Countdown::kPreparationSeconds == 30u);
    static_assert(Countdown::kRoundStartScreenSeconds == 8u);

    Countdown countdown(1u);
    EXPECT_FALSE(countdown.Advance(
        Countdown::Phase::Preparation, 30.0f).deployPreparedClients);

    const auto earlyClient = countdown.SyncClient(
        10u, Countdown::Phase::Preparation, 30.0f);
    EXPECT_FALSE(earlyClient.showRoundStartScreen);
    EXPECT_FALSE(earlyClient.hideRoundStartScreen);

    EXPECT_TRUE(countdown.Advance(
        Countdown::Phase::Preparation, 8.0f).deployPreparedClients);
    EXPECT_FALSE(countdown.Advance(
        Countdown::Phase::Preparation, 7.9f).deployPreparedClients);

    const auto show = countdown.SyncClient(
        10u, Countdown::Phase::Preparation, 8.0f);
    EXPECT_TRUE(show.showRoundStartScreen);
    EXPECT_FALSE(show.hideRoundStartScreen);
    EXPECT_EQ(show.displaySeconds, 8u);

    const auto duplicateShow = countdown.SyncClient(
        10u, Countdown::Phase::Preparation, 7.0f);
    EXPECT_FALSE(duplicateShow.showRoundStartScreen);
    EXPECT_FALSE(duplicateShow.hideRoundStartScreen);

    const auto hide = countdown.SyncClient(
        10u, Countdown::Phase::Active, 1200.0f);
    EXPECT_FALSE(hide.showRoundStartScreen);
    EXPECT_TRUE(hide.hideRoundStartScreen);
    const auto duplicateHide = countdown.SyncClient(
        10u, Countdown::Phase::Active, 1199.0f);
    EXPECT_FALSE(duplicateHide.hideRoundStartScreen);
}

TEST(DeploymentCountdown, ReconnectReceivesCurrentWindowOrDefensiveActiveHide) {
    Countdown countdown(4u);
    countdown.Advance(Countdown::Phase::Preparation, 8.0f);

    const auto firstShow = countdown.SyncClient(
        22u, Countdown::Phase::Preparation, 4.01f);
    EXPECT_TRUE(firstShow.showRoundStartScreen);
    EXPECT_EQ(firstShow.displaySeconds, 5u);

    countdown.RemoveClient(22u);
    const auto reconnectShow = countdown.SyncClient(
        22u, Countdown::Phase::Preparation, 3.2f);
    EXPECT_TRUE(reconnectShow.showRoundStartScreen);
    EXPECT_EQ(reconnectShow.displaySeconds, 4u);

    countdown.RemoveClient(22u);
    const auto reconnectActive = countdown.SyncClient(
        22u, Countdown::Phase::Active, 1190.0f);
    EXPECT_FALSE(reconnectActive.showRoundStartScreen);
    EXPECT_TRUE(reconnectActive.hideRoundStartScreen);
    EXPECT_FALSE(countdown.SyncClient(
        22u, Countdown::Phase::Active, 1189.0f).hideRoundStartScreen);

    countdown.RemoveClient(23u);
    const auto reconnectEarly = countdown.SyncClient(
        23u, Countdown::Phase::Preparation, 9.0f);
    EXPECT_FALSE(reconnectEarly.showRoundStartScreen);
    EXPECT_FALSE(reconnectEarly.hideRoundStartScreen);
}

TEST(DeploymentCountdown, CoarseZeroCrossingAndGenerationResetRemainOneShot) {
    Countdown countdown(8u);

    // A coarse server tick must not skip prepared deployment or active cleanup.
    EXPECT_TRUE(countdown.Advance(
        Countdown::Phase::Preparation, 0.0f).deployPreparedClients);
    EXPECT_FALSE(countdown.Advance(
        Countdown::Phase::Active, 1200.0f).deployPreparedClients);
    EXPECT_TRUE(countdown.SyncClient(
        31u, Countdown::Phase::Preparation, 0.0f).hideRoundStartScreen);
    EXPECT_FALSE(countdown.SyncClient(
        31u, Countdown::Phase::Active, 1200.0f).hideRoundStartScreen);

    countdown.ResetForGeneration(9u);
    EXPECT_EQ(countdown.GetGeneration(), 9u);
    EXPECT_TRUE(countdown.Advance(
        Countdown::Phase::Preparation, 8.0f).deployPreparedClients);
    EXPECT_TRUE(countdown.SyncClient(
        31u, Countdown::Phase::Preparation, 2.0f).showRoundStartScreen);

    // Reset remains explicit and re-arms actions even with a reused token.
    countdown.ResetForGeneration(9u);
    EXPECT_TRUE(countdown.Advance(
        Countdown::Phase::Active, 1200.0f).deployPreparedClients);
    const auto activeSync = countdown.SyncClient(
        31u, Countdown::Phase::Active, 1200.0f);
    EXPECT_TRUE(activeSync.hideRoundStartScreen);
    EXPECT_FALSE(activeSync.showRoundStartScreen);
}

TEST(DeploymentCountdown, NonFinitePreparationTimeDoesNotOpenOrDeploy) {
    Countdown countdown;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    EXPECT_FALSE(countdown.Advance(
        Countdown::Phase::Preparation, nan).deployPreparedClients);
    const auto sync = countdown.SyncClient(
        44u, Countdown::Phase::Preparation, nan);
    EXPECT_FALSE(sync.showRoundStartScreen);
    EXPECT_FALSE(sync.hideRoundStartScreen);

    // Phase authority still wins over a bad timer once the game is Active.
    EXPECT_TRUE(countdown.Advance(
        Countdown::Phase::Active, nan).deployPreparedClients);
    EXPECT_TRUE(countdown.SyncClient(
        44u, Countdown::Phase::Active, nan).hideRoundStartScreen);
}

} // namespace

RS2V_TEST_MAIN()
