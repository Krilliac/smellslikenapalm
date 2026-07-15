#include "TestFramework.h"

#include "Game/Player.h"
#include "Game/PlayerManager.h"
#include "Network/ClientConnection.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

std::shared_ptr<Player> ConnectTestPlayer(PlayerManager& players, uint32_t clientId) {
    auto connection = std::make_shared<ClientConnection>(
        clientId, "127.0.0.1", uint16_t{7777}, nullptr, nullptr);
    // The lifecycle fixture intentionally has no socket or GameServer. Marking it
    // UE3 suppresses the unrelated legacy Packet send path during state changes.
    connection->SetUE3Client(true);
    connection->SetPlayerName("LifecycleTest");
    connection->SetTeamId(1u);
    players.OnPlayerConnect(connection);
    return players.GetPlayer(clientId);
}

TEST(PlayerManagerLifecycle, LethalHealthThenDeathNotificationDoesNotRestartTimer) {
    PlayerManager players(nullptr);
    auto player = ConnectTestPlayer(players, 41u);
    ASSERT_NE(player, nullptr);

    players.OnPlayerSpawn(41u);
    ASSERT_EQ(player->GetState(), PlayerState::Alive);

    // This is the production ordering: Player::SetHealth marks the death first,
    // then DamageSystem/CombatAuthority notifies PlayerManager.
    player->SetHealth(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    players.OnPlayerDeath(41u);

    // If the notification re-stamped death time, this would still be false.
    EXPECT_TRUE(player->CanRespawn(1));
    EXPECT_EQ(player->GetState(), PlayerState::Dead);
    EXPECT_EQ(player->GetHealth(), 0);
}

TEST(PlayerManagerLifecycle, DirectDeathNotificationProducesCoherentDeadState) {
    PlayerManager players(nullptr);
    auto player = ConnectTestPlayer(players, 42u);
    ASSERT_NE(player, nullptr);

    players.OnPlayerSpawn(42u);
    ASSERT_EQ(player->GetHealth(), 100);
    players.OnPlayerDeath(42u);

    EXPECT_EQ(player->GetState(), PlayerState::Dead);
    EXPECT_EQ(player->GetHealth(), 0);
}

TEST(PlayerManagerLifecycle, DuplicateSpawnDoesNotRefillHealthOrReplayTransition) {
    PlayerManager players(nullptr);
    auto player = ConnectTestPlayer(players, 43u);
    ASSERT_NE(player, nullptr);

    players.OnPlayerSpawn(43u);
    player->SetHealth(37);
    players.OnPlayerSpawn(43u);

    EXPECT_EQ(player->GetState(), PlayerState::Alive);
    EXPECT_EQ(player->GetHealth(), 37);
}

TEST(PlayerManagerLifecycle, SpectatorAndUnknownLifecycleEventsFailClosed) {
    PlayerManager players(nullptr);
    auto player = ConnectTestPlayer(players, 44u);
    ASSERT_NE(player, nullptr);

    player->SetState(PlayerState::Spectating);
    players.OnPlayerSpawn(44u);
    players.OnPlayerDeath(44u);
    EXPECT_EQ(player->GetState(), PlayerState::Spectating);

    EXPECT_NO_THROW(players.OnPlayerSpawn(999u));
    EXPECT_NO_THROW(players.OnPlayerDeath(999u));
    EXPECT_NO_THROW(players.OnPlayerConnect(nullptr));
}

} // namespace

RS2V_TEST_MAIN()
