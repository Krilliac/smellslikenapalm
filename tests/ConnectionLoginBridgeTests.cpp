#include "TestFramework.h"

#include "Game/ConnectionLoginBridge.h"
#include "Game/GameServer.h"
#include "Game/Player.h"
#include "Game/PlayerManager.h"
#include "Network/ClientConnection.h"

#include <memory>

TEST(ConnectionLoginBridge, JoinKeepsMenuPlayerUndeployedUntilRoleSelection) {
    GameServer server;
    PlayerManager players(&server);
    players.Initialize();

    auto connection = std::make_shared<ClientConnection>(
        42u, "127.0.0.1", 7777u, nullptr, nullptr);
    // Suppress the legacy Packet path; this unit fixture intentionally has no
    // socket because it exercises game-layer lifecycle only.
    connection->SetUE3Client(true);

    ConnectionLoginBridge::Dependencies deps;
    deps.playerManager = &players;
    deps.resolveConnection = [connection](uint32_t clientId) {
        return clientId == connection->GetClientId() ? connection : nullptr;
    };
    ConnectionLoginBridge bridge(std::move(deps));

    ClientLoggedInEvent login;
    login.clientId = 42u;
    login.options = URLOptions::Parse("VNTE-Resort?Name=MenuPlayer?Team=1");
    bridge.OnClientLoggedIn(login);
    bridge.OnClientJoined(ClientJoinedEvent{42u});

    const auto player = players.GetPlayer(42u);
    ASSERT_NE(player, nullptr);
    EXPECT_EQ(player->GetTeam(), 1u);          // server US team
    EXPECT_EQ(player->GetState(), PlayerState::Dead);
    EXPECT_FALSE(player->IsAlive());
    EXPECT_FALSE(player->IsReadyToSpawn());
    EXPECT_FALSE(bridge.WasSpawnAttempted(42u));
}

RS2V_TEST_MAIN()
