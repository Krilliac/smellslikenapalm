#include "TestFramework.h"

#include "Config/ConfigManager.h"
#include "Config/MapConfig.h"
#include "Config/ServerConfig.h"
#include "Game/CommandManager.h"
#include "Game/GameServer.h"
#include "Game/MapManager.h"
#include "Game/MapVoteManager.h"
#include "Game/RoleSystem.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class GameServerNativeRotationTestHarness {
public:
    static void Observe(GameServer& server,
                        bool nativeDriverOwnsRound,
                        bool nativeModePresent,
                        bool finished) {
        server.ObserveNativeModeCompletion(nativeDriverOwnsRound,
                                           nativeModePresent,
                                           finished);
    }

    static bool Pending(const GameServer& server) {
        return server.m_mapChangeRequested;
    }

    static bool Latched(const GameServer& server) {
        return server.m_nativeModeMapChangeLatched;
    }

    static bool Consume(GameServer& server) {
        return server.ConsumeDeferredMapChange();
    }

    static void InstallNextDriver(GameServer& server) {
        server.ResetNativeModeMapChangeLatch();
    }

    static void InstallMapManagerWithoutConfig(GameServer& server) {
        server.m_mapManager = std::make_unique<MapManager>(&server, nullptr);
    }

    static void InstallMapRuntime(GameServer& server,
                                  std::shared_ptr<MapConfig> mapConfig) {
        server.m_mapConfig = std::move(mapConfig);
        server.m_mapManager =
            std::make_unique<MapManager>(&server, server.m_mapConfig);
        server.m_mapVoteManager =
            std::make_unique<MapVoteManager>(server.m_mapConfig);
    }

    static RoleSystem& InstallRoleSystem(GameServer& server) {
        server.m_roleSystem = std::make_unique<RoleSystem>(&server);
        server.m_roleSystem->Initialize();
        return *server.m_roleSystem;
    }

    static void ActivateRoleAuthority(GameServer& server,
                                      const std::string& mapName) {
        server.ActivateRoleAuthorityForMap(mapName);
    }

    static void SetSquadServerSettings(GameServer& server, int maxPlayers,
                                       const std::string& gameMode) {
        if (!server.m_configManager) {
            server.m_configManager = std::make_shared<ConfigManager>();
            server.m_serverConfig =
                std::make_shared<ServerConfig>(server.m_configManager);
        }
        server.m_configManager->SetInt("General.max_players", maxPlayers);
        server.m_configManager->SetString("Game.game_mode", gameMode);
    }

    static void InstallVoteWinner(GameServer& server, std::string target) {
        server.m_pendingVoteWinner = std::move(target);
    }

    static const std::string& CurrentMap(const GameServer& server) {
        return server.m_mapManager->GetCurrentMapName();
    }

    static bool HasPendingVoteWinner(const GameServer& server) {
        return !server.m_pendingVoteWinner.empty();
    }

    static const std::string& PendingRequestedMap(const GameServer& server) {
        return server.m_pendingRequestedMap;
    }
};

TEST(GameServerNativeRotation,
     MapActivationResetsRetailSquadsAndAppliesExactFactions) {
    GameServer server;
    RoleSystem& roles =
        GameServerNativeRotationTestHarness::InstallRoleSystem(server);

    const auto activateAndExpect =
        [&](const std::string& mapName, Faction teamOne, Faction teamTwo,
            uint32_t playerId) {
            const RetailSquadAssignment before =
                roles.AutoAssignRetailSquad(playerId, 1);
            ASSERT_TRUE(before.IsValid());

            GameServerNativeRotationTestHarness::ActivateRoleAuthority(
                server, mapName);

            EXPECT_FALSE(roles.GetRetailSquadAssignment(playerId).has_value());
            EXPECT_NE(roles.GetRetailSquadGeneration(), before.generation);
            EXPECT_EQ(roles.GetRetailSquad(1, 0)->Occupancy(), 0u);
            EXPECT_EQ(roles.GetActiveRetailSquadCount(), 10u);
            EXPECT_EQ(roles.GetTeamFaction(1), teamOne);
            EXPECT_EQ(roles.GetTeamFaction(2), teamTwo);
        };

    activateAndExpect("VNSK-Compound", Faction::USMC, Faction::NLFSV, 11);
    activateAndExpect("VNTE-CuChi", Faction::USArmy, Faction::NLFSV, 12);

    // An unoverridden map must actively restore both defaults rather than
    // inheriting Compound or Cu Chi's faction state.
    activateAndExpect("VNTE-Resort", Faction::USArmy, Faction::NVA, 13);
}

TEST(GameServerNativeRotation,
     MapActivationConfiguresRetailSquadsFromCapacityAndMode) {
    GameServer server;
    RoleSystem& roles =
        GameServerNativeRotationTestHarness::InstallRoleSystem(server);

    GameServerNativeRotationTestHarness::SetSquadServerSettings(
        server, 24, "Skirmish");
    GameServerNativeRotationTestHarness::ActivateRoleAuthority(
        server, "VNSK-Compound");
    EXPECT_EQ(roles.GetActiveRetailSquadCount(), 2u);
    EXPECT_TRUE(roles.JoinRetailSquad(1, 1, 1).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(2, 1, 2).IsValid());

    GameServerNativeRotationTestHarness::SetSquadServerSettings(
        server, 24, "Territories");
    GameServerNativeRotationTestHarness::ActivateRoleAuthority(
        server, "VNTE-CuChi");
    EXPECT_EQ(roles.GetActiveRetailSquadCount(), 4u);
    EXPECT_FALSE(roles.GetRetailSquadAssignment(1).has_value());
    EXPECT_TRUE(roles.JoinRetailSquad(3, 1, 3).IsValid());
    EXPECT_FALSE(roles.JoinRetailSquad(4, 1, 4).IsValid());

    // Invalid configured capacity uses the safe 64-player default.
    GameServerNativeRotationTestHarness::SetSquadServerSettings(
        server, 0, "Skirmish");
    GameServerNativeRotationTestHarness::ActivateRoleAuthority(
        server, "VNSK-Compound");
    EXPECT_EQ(roles.GetActiveRetailSquadCount(), 10u);
}

TEST(GameServerNativeRotation, TerminalObservationQueuesButDoesNotChangeInline) {
    GameServer server;

    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(server));
    GameServerNativeRotationTestHarness::Observe(
        server, true, true, true);

    // Observation only requests work. The explicit deferred consumer has not
    // run yet, so a mode can never delete itself from its own Update stack.
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Pending(server));
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Latched(server));

    // No MapManager is installed in this harness. The safe failed attempt
    // consumes the request but deliberately leaves the terminal latch set.
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Consume(server));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(server));
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Latched(server));

    GameServerNativeRotationTestHarness::Observe(
        server, true, true, true);
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(server));
}

TEST(GameServerNativeRotation, NonOwnerNullAndNonTerminalStatesNeverLatch) {
    GameServer server;

    // Generic/central ownership, a missing native pointer, and a live native
    // phase are all non-events even if another input claims Finished.
    GameServerNativeRotationTestHarness::Observe(
        server, false, true, true);
    GameServerNativeRotationTestHarness::Observe(
        server, true, false, true);
    GameServerNativeRotationTestHarness::Observe(
        server, true, true, false);

    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(server));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Latched(server));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Consume(server));
}

TEST(GameServerNativeRotation, ShutdownDropsQueuedWorkAndRejectsNewRequests) {
    GameServer queued;
    GameServerNativeRotationTestHarness::Observe(queued, true, true, true);
    ASSERT_TRUE(GameServerNativeRotationTestHarness::Pending(queued));
    queued.RequestShutdown();

    EXPECT_FALSE(GameServerNativeRotationTestHarness::Consume(queued));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(queued));
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Latched(queued));

    GameServer alreadyStopping;
    alreadyStopping.RequestShutdown();
    GameServerNativeRotationTestHarness::Observe(
        alreadyStopping, true, true, true);
    alreadyStopping.RequestMapChange();
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(alreadyStopping));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Latched(alreadyStopping));
}

TEST(GameServerNativeRotation, InstallingNextDriverRearmsExactlyOnce) {
    GameServer server;
    GameServerNativeRotationTestHarness::Observe(server, true, true, true);
    ASSERT_TRUE(GameServerNativeRotationTestHarness::Consume(server));
    ASSERT_TRUE(GameServerNativeRotationTestHarness::Latched(server));

    GameServerNativeRotationTestHarness::InstallNextDriver(server);
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Latched(server));
    GameServerNativeRotationTestHarness::Observe(server, true, true, true);
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Pending(server));
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Latched(server));
}

TEST(GameServerNativeRotation, UnsupportedVoteTargetsFailBeforeMapMutation) {
    for (const char* target : {"VNTE-RungSac", "VNTE-Hill937"}) {
        GameServer server;
        GameServerNativeRotationTestHarness::InstallMapManagerWithoutConfig(server);
        GameServerNativeRotationTestHarness::InstallVoteWinner(server, target);

        ASSERT_TRUE(
            GameServerNativeRotationTestHarness::HasPendingVoteWinner(server));
        server.ChangeMap();

        // The one-shot requested target is consumed, but profile preflight
        // returns before LoadMap can touch the current world identity.
        EXPECT_FALSE(
            GameServerNativeRotationTestHarness::HasPendingVoteWinner(server));
        EXPECT_TRUE(
            GameServerNativeRotationTestHarness::CurrentMap(server).empty());
    }
}

TEST(GameServerNativeRotation, AdminCommandQueuesValidatedDeferredTarget) {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("rs2v_admin_map_change_" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    const std::filesystem::path rotationFile = root / "config" / "maps.ini";
    std::filesystem::create_directories(rotationFile.parent_path());
    {
        std::ofstream out(rotationFile);
        out << "[VNTE-Resort]\n"
               "display_name=Resort\n"
               "file=" << (root / "VNTE-Resort.roe").generic_string() << "\n"
               "default_mode=Territories\n"
               "supported_modes=Territories\n\n"
               "[VNTE-RungSac]\n"
               "display_name=Rung Sac\n"
               "file=" << (root / "VNTE-RungSac.roe").generic_string() << "\n"
               "default_mode=Territories\n"
               "supported_modes=Territories\n";
    }

    auto configManager = std::make_shared<ConfigManager>();
    configManager->SetString("General.data_directory", root.generic_string());
    configManager->SetString(
        "DataPaths.maps_path", (root / "maps").generic_string());
    configManager->SetString(
        "General.map_rotation_file", rotationFile.generic_string());
    ServerConfig serverConfig(configManager);
    auto mapConfig = std::make_shared<MapConfig>(serverConfig);
    ASSERT_TRUE(mapConfig->Initialize());

    const auto makeContext = [](GameServer& server,
                                std::vector<std::string>& lines) {
        CommandContext ctx;
        ctx.level = CommandLevel::Admin;
        ctx.source = CommandSource::Console;
        ctx.invoker = "test-admin";
        ctx.server = &server;
        ctx.out = [&lines](std::string_view line) {
            lines.emplace_back(line);
        };
        return ctx;
    };
    const auto contains = [](const std::vector<std::string>& lines,
                             std::string_view needle) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    };

    GameServer queued;
    GameServerNativeRotationTestHarness::InstallMapRuntime(queued, mapConfig);
    ASSERT_TRUE(queued.GetMapVoteManager()->StartVote("").size() == 1u);
    ASSERT_TRUE(queued.GetMapVoteManager()->IsVoteActive());
    CommandManager queuedCommands(&queued);
    queuedCommands.Initialize();

    std::vector<std::string> acceptedLines;
    CommandContext accepted = makeContext(queued, acceptedLines);
    EXPECT_TRUE(queuedCommands.Execute(accepted, "changemap VNTE-Resort"));
    EXPECT_TRUE(contains(acceptedLines, "Map change queued: VNTE-Resort"));
    EXPECT_FALSE(queued.GetMapVoteManager()->IsVoteActive());
    EXPECT_EQ(
        GameServerNativeRotationTestHarness::PendingRequestedMap(queued),
        std::string("VNTE-Resort"));
    EXPECT_TRUE(GameServerNativeRotationTestHarness::Pending(queued));
    EXPECT_TRUE(
        GameServerNativeRotationTestHarness::CurrentMap(queued).empty());

    std::vector<std::string> duplicateLines;
    CommandContext duplicate = makeContext(queued, duplicateLines);
    EXPECT_FALSE(queuedCommands.Execute(duplicate, "changemap VNTE-Resort"));
    EXPECT_TRUE(contains(duplicateLines, "already queued"));

    GameServer rejected;
    GameServerNativeRotationTestHarness::InstallMapRuntime(rejected, mapConfig);
    CommandManager rejectedCommands(&rejected);
    rejectedCommands.Initialize();

    std::vector<std::string> unsupportedLines;
    CommandContext unsupported = makeContext(rejected, unsupportedLines);
    EXPECT_FALSE(
        rejectedCommands.Execute(unsupported, "changemap VNTE-RungSac"));
    EXPECT_TRUE(contains(unsupportedLines, "no exact source-grounded"));
    EXPECT_FALSE(GameServerNativeRotationTestHarness::Pending(rejected));
    EXPECT_TRUE(
        GameServerNativeRotationTestHarness::PendingRequestedMap(rejected)
            .empty());

    std::vector<std::string> unknownLines;
    CommandContext unknown = makeContext(rejected, unknownLines);
    EXPECT_FALSE(rejectedCommands.Execute(unknown, "changemap VNTE-Unknown"));
    EXPECT_TRUE(contains(unknownLines, "not present in the configured rotation"));

    GameServer stopping;
    GameServerNativeRotationTestHarness::InstallMapRuntime(stopping, mapConfig);
    stopping.RequestShutdown();
    CommandManager stoppingCommands(&stopping);
    stoppingCommands.Initialize();
    std::vector<std::string> stoppingLines;
    CommandContext stoppingContext = makeContext(stopping, stoppingLines);
    EXPECT_FALSE(
        stoppingCommands.Execute(stoppingContext, "changemap VNTE-Resort"));
    EXPECT_TRUE(contains(stoppingLines, "server is shutting down"));
}

RS2V_TEST_MAIN()
