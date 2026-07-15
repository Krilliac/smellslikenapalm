#include "TestFramework.h"

#include "Game/BotManager.h"
#include "Game/CombatAuthority.h"
#include "Game/GameServer.h"
#include "Game/ObjectiveSystem.h"
#include "Game/Player.h"
#include "Game/PlayerManager.h"
#include "Game/SkirmishMode.h"
#include "Game/SpawnSystem.h"
#include "Game/SupremacyMode.h"
#include "Game/TeamManager.h"
#include "Game/TicketSystem.h"
#include "Network/ClientConnection.h"
#include "Network/GameplayRpcReplication.h"
#include "Network/Packet.h"
#include "Network/SocketFactory.h"
#include "Network/UDPSocket.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// GameServer owns the coordinator being tested and its concrete dependencies.
// This friend installs those same subsystems without binding the game/EAC
// listeners or constructing unrelated map/mode services.
class GameServerBotCombatTestHarness {
public:
    static void Install(GameServer& server, const BotManagerConfig& config) {
        server.m_combatAuthority =
            std::make_unique<CombatAuthority::Authority>();
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(10, 10);
        server.m_botManager = std::make_unique<BotManager>(config);
        server.m_botCombatGeneration = config.humanCombatGeneration;
        server.m_playerManager = std::make_unique<PlayerManager>(&server);
        server.m_playerManager->Initialize();
    }

    static void ReplaceBots(GameServer& server,
                            const BotManagerConfig& config) {
        server.m_botManager = std::make_unique<BotManager>(config);
        server.m_botCombatGeneration = config.humanCombatGeneration;
    }

    static bool Process(GameServer& server,
                        const BotHumanCombatEvent& event) {
        return server.ProcessBotHumanCombatEvent(event);
    }

    static bool HasM61VisualProjectile(const GameServer& server,
                                       std::uint64_t projectileId) {
        return server.m_retailM61Projectiles.contains(projectileId);
    }

    static void InstallMutualTicketVolley(
        GameServer& server, const BotManagerConfig& config,
        std::size_t& batchCallbacks,
        std::vector<std::size_t>& batchSizes,
        std::vector<std::uint8_t>& firstVictimTeams) {
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(1, 1);
        server.m_botManager = std::make_unique<BotManager>(config);
        server.m_botCombatGeneration = config.humanCombatGeneration;
        server.m_skirmishMode = std::make_unique<SkirmishMode>(&server);
        server.m_activeModeDriver = GameServer::ActiveModeDriver::Skirmish;

        server.m_ticketSystem->SetOnTicketsDepleted(
            [&server](uint32_t teamId) {
                if (server.m_skirmishMode) {
                    server.m_skirmishMode->OnTicketsDepleted(teamId);
                }
            });
        server.m_botManager->SetDeathBatchCallback(
            [&server, &batchCallbacks, &batchSizes, &firstVictimTeams](
                const std::vector<BotDeathEvent>& deaths) {
                ++batchCallbacks;
                batchSizes.push_back(deaths.size());
                if (!deaths.empty()) {
                    firstVictimTeams.push_back(deaths.front().teamId);
                }
                server.ProcessBotDeathBatch(deaths);
            });
    }

    static void InstallSupremacyMutualTicketVolley(
        GameServer& server, const BotManagerConfig& config,
        std::size_t& batchCallbacks,
        std::vector<std::size_t>& batchSizes,
        std::vector<std::uint8_t>& firstVictimTeams) {
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(1, 1);
        server.m_botManager = std::make_unique<BotManager>(config);
        server.m_botCombatGeneration = config.humanCombatGeneration;
        server.m_supremacyMode = std::make_unique<SupremacyMode>(&server);
        server.m_activeModeDriver = GameServer::ActiveModeDriver::Supremacy;

        server.m_ticketSystem->SetOnTicketsDepleted(
            [&server](uint32_t teamId) {
                if (server.m_supremacyMode) {
                    server.m_supremacyMode->OnTicketsDepleted(teamId);
                }
            });
        server.m_botManager->SetDeathBatchCallback(
            [&server, &batchCallbacks, &batchSizes, &firstVictimTeams](
                const std::vector<BotDeathEvent>& deaths) {
                ++batchCallbacks;
                batchSizes.push_back(deaths.size());
                if (!deaths.empty()) {
                    firstVictimTeams.push_back(deaths.front().teamId);
                }
                server.ProcessBotDeathBatch(deaths);
            });
    }

    static void InstallSupremacyRespawnGate(
        GameServer& server, const BotManagerConfig& config) {
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(1, 1);

        server.m_botManager = std::make_unique<BotManager>(config);
        server.m_botCombatGeneration = config.humanCombatGeneration;
        server.m_botManager->SetTeamRespawnAllowed(
            BotManager::kTeamOne, false);
        server.m_botManager->SetTeamRespawnAllowed(
            BotManager::kTeamTwo, false);

        server.m_spawnSystem = std::make_unique<SpawnSystem>(&server);
        server.m_spawnSystem->Initialize();
        SpawnLocation southSpawn;
        southSpawn.type = SpawnType::BaseSpawn;
        southSpawn.name = "South Base";
        southSpawn.teamId = BotManager::kTeamOne;
        southSpawn.position = Vector3(-100.0f, 0.0f, 0.0f);
        server.m_spawnSystem->AddSpawnLocation(southSpawn);
        SpawnLocation northSpawn;
        northSpawn.type = SpawnType::BaseSpawn;
        northSpawn.name = "North Base";
        northSpawn.teamId = BotManager::kTeamTwo;
        northSpawn.position = Vector3(100.0f, 0.0f, 0.0f);
        server.m_spawnSystem->AddSpawnLocation(northSpawn);

        server.m_objectiveSystem = std::make_unique<ObjectiveSystem>(&server);
        server.m_objectiveSystem->Initialize();
        server.m_supremacyMode = std::make_unique<SupremacyMode>(&server);
        server.m_supremacyMode->Initialize();
        server.m_supremacyMode->StartRound();
        server.m_supremacyMode->Update(30.0f);
        server.m_ticketSystem->SetTickets(BotManager::kTeamOne, 0);
        server.m_activeModeDriver = GameServer::ActiveModeDriver::Supremacy;
    }

    static void InstallSupremacyTicketLifecycle(
        GameServer& server, uint32_t teamOneTickets,
        uint32_t teamTwoTickets) {
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(teamOneTickets, teamTwoTickets);
        server.m_supremacyMode = std::make_unique<SupremacyMode>(&server);
        server.m_activeModeDriver = GameServer::ActiveModeDriver::Supremacy;
        server.m_ticketSystem->SetOnTicketsDepleted(
            [&server](uint32_t teamId) {
                if (server.m_supremacyMode) {
                    server.m_supremacyMode->OnTicketsDepleted(teamId);
                }
            });
        server.m_supremacyMode->Initialize();
    }

    static void InstallSupremacyHumanTicketVolley(GameServer& server) {
        InstallSupremacyTicketLifecycle(server, 1, 1);
        server.m_playerManager = std::make_unique<PlayerManager>(&server);
        server.m_playerManager->Initialize();
        server.m_teamManager = std::make_unique<TeamManager>(&server);
        server.m_teamManager->Initialize();

        const auto connect = [&server](uint32_t clientId, uint32_t teamId) {
            auto connection = std::make_shared<ClientConnection>(
                clientId, "127.0.0.1", uint16_t{7777}, nullptr, nullptr);
            connection->SetUE3Client(true);
            connection->SetPlayerName("SupremacyVolley");
            connection->SetTeamId(teamId);
            server.m_playerManager->OnPlayerConnect(connection);
            server.m_teamManager->AddPlayerToTeam(clientId, teamId);
            server.m_playerManager->OnPlayerSpawn(clientId);
        };
        connect(101u, BotManager::kTeamOne);
        connect(202u, BotManager::kTeamTwo);
    }

    static void ProcessCombatEvents(
        GameServer& server,
        const std::vector<CombatAuthority::CombatEvent>& events) {
        server.ProcessCombatEvents(events);
    }

    static void RefreshBotWorldState(GameServer& server) {
        server.RefreshBotWorldState();
    }

    static void Reset(GameServer& server) {
        // PlayerManager teardown calls back into GameServer, so destroy it
        // while the authority member is still a live (possibly populated)
        // unique_ptr. The remaining members then have no callbacks into each
        // other.
        server.m_skirmishMode.reset();
        server.m_supremacyMode.reset();
        server.m_playerManager.reset();
        server.m_teamManager.reset();
        server.m_objectiveSystem.reset();
        server.m_spawnSystem.reset();
        server.m_botManager.reset();
        server.m_ticketSystem.reset();
        server.m_combatAuthority.reset();
    }
};

namespace {

constexpr std::uint32_t kHumanClientId = 42;
constexpr std::uint64_t kInitialGeneration = 41;

BotManagerConfig CombatConfig(std::uint64_t generation) {
    BotManagerConfig config;
    config.fillTargetPerTeam = 1;
    config.maxBotsPerTeam = 2;
    config.fixedStepSeconds = 0.1f;
    config.moveSpeed = 0.0f;
    config.respawnDelaySeconds = 0.2f;
    config.maxHealth = 100.0f;
    config.captureWeight = 1.0f;
    config.objectiveArrivalTolerance = 0.1f;
    config.maxCatchUpSteps = 4;
    config.combatRangeUu = 1000.0f;
    config.combatDamage = 60.0f;
    config.combatRoundsPerMinute = 600.0f;
    config.humanCombatGeneration = generation;
    return config;
}

void PrepareBots(BotManager& bots) {
    bots.SetEligibleSpawns({
        BotSpawnSnapshot{1, BotManager::kTeamOne,
                         Vector3(100.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{2, BotManager::kTeamTwo,
                         Vector3(-100.0f, 0.0f, 0.0f)},
    });
    // The team-two human occupies that side's sole fill slot, leaving exactly
    // one team-one bot and no competing bot target.
    bots.SetHumanTeamCounts(0, 1);
    bots.ConsumeRespawnEvents();
}

struct MutualVolleyResult {
    bool updated = false;
    std::size_t batchCallbacks = 0;
    std::vector<std::size_t> batchSizes;
    std::vector<std::uint8_t> firstVictimTeams;
    uint32_t southTickets = 0;
    uint32_t northTickets = 0;
    SkirmishMode::Phase phase = SkirmishMode::Phase::WarmUp;
    int southWins = 0;
    int northWins = 0;
};

struct SupremacyVolleyResult {
    bool updated = false;
    std::size_t batchCallbacks = 0;
    std::vector<std::size_t> batchSizes;
    std::vector<std::uint8_t> firstVictimTeams;
    uint32_t southTickets = 0;
    uint32_t northTickets = 0;
    SupremacyMode::Phase phase = SupremacyMode::Phase::WarmUp;
    uint32_t winningTeam = 0;
};

MutualVolleyResult RunMutualLastTicketVolley(bool northVictimPublishesFirst) {
    BotManagerConfig config = CombatConfig(kInitialGeneration + 100);
    config.combatDamage = 100.0f;

    GameServer server;
    MutualVolleyResult result;
    GameServerBotCombatTestHarness::InstallMutualTicketVolley(
        server, config, result.batchCallbacks, result.batchSizes,
        result.firstVictimTeams);

    BotManager* bots = server.GetBotManager();
    bots->SetEligibleSpawns({
        BotSpawnSnapshot{1, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{2, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    if (northVictimPublishesFirst) {
        // Recreate South after North so the map-ordered death transaction is
        // [North Bot(2), South Bot(3)] instead of the default reverse order.
        bots->SetHumanTeamCount(BotManager::kTeamOne, 1);
        bots->SetHumanTeamCount(BotManager::kTeamOne, 0);
    }
    bots->ConsumeRespawnEvents();

    SkirmishMode* mode = server.GetSkirmishMode();
    mode->Initialize();
    mode->StartRound();
    mode->Update(15.0f);
    result.updated = bots->Update(0.1f);

    result.southTickets =
        server.GetTicketSystem()->GetTickets(BotManager::kTeamOne);
    result.northTickets =
        server.GetTicketSystem()->GetTickets(BotManager::kTeamTwo);
    result.phase = mode->GetPhase();
    result.southWins = mode->GetTeamRoundWins(BotManager::kTeamOne);
    result.northWins = mode->GetTeamRoundWins(BotManager::kTeamTwo);

    GameServerBotCombatTestHarness::Reset(server);
    return result;
}

SupremacyVolleyResult RunSupremacyMutualLastTicketVolley(
    bool northVictimPublishesFirst) {
    BotManagerConfig config = CombatConfig(kInitialGeneration + 200);
    config.combatDamage = 100.0f;

    GameServer server;
    SupremacyVolleyResult result;
    GameServerBotCombatTestHarness::InstallSupremacyMutualTicketVolley(
        server, config, result.batchCallbacks, result.batchSizes,
        result.firstVictimTeams);

    BotManager* bots = server.GetBotManager();
    bots->SetEligibleSpawns({
        BotSpawnSnapshot{1, BotManager::kTeamOne,
                         Vector3(0.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{2, BotManager::kTeamTwo,
                         Vector3(0.0f, 0.0f, 0.0f)},
    });
    if (northVictimPublishesFirst) {
        bots->SetHumanTeamCount(BotManager::kTeamOne, 1);
        bots->SetHumanTeamCount(BotManager::kTeamOne, 0);
    }
    bots->ConsumeRespawnEvents();

    SupremacyMode* mode = server.GetSupremacyMode();
    mode->Initialize();
    mode->StartRound();
    mode->Update(30.0f);
    result.updated = bots->Update(0.1f);
    // Production drains the complete bot death transaction before the native
    // mode Update. Winner selection must observe that same stable boundary.
    mode->Update(0.0f);

    result.southTickets =
        server.GetTicketSystem()->GetTickets(BotManager::kTeamOne);
    result.northTickets =
        server.GetTicketSystem()->GetTickets(BotManager::kTeamTwo);
    result.phase = mode->GetPhase();
    result.winningTeam = mode->GetWinningTeam();

    GameServerBotCombatTestHarness::Reset(server);
    return result;
}

class GameServerBotCombatTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(SocketFactory::Initialize());

        GameServerBotCombatTestHarness::Install(
            server_, CombatConfig(kInitialGeneration));
        PrepareBots(*server_.GetBotManager());

        receiverPort_ = BindReceiver();
        ASSERT_TRUE(receiverPort_ != 0);
        sender_ = std::make_shared<UDPSocket>();
        ASSERT_TRUE(sender_->Bind(0));

        connection_ = std::make_shared<ClientConnection>(
            kHumanClientId, "127.0.0.1", receiverPort_, sender_, nullptr);
        connection_->SetPlayerName("CombatTarget");
        connection_->SetTeamId(BotManager::kTeamTwo);
        server_.GetPlayerManager()->OnPlayerConnect(connection_);
        server_.GetPlayerManager()->OnPlayerSpawn(kHumanClientId);

        const std::vector<int> initialHealth = WaitForHealth();
        ASSERT_EQ(initialHealth.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(initialHealth.front(), 100);

        const std::shared_ptr<Player> player =
            server_.GetPlayerManager()->GetPlayer(kHumanClientId);
        ASSERT_TRUE(player != nullptr);
        ASSERT_TRUE(player->IsAlive());
        ASSERT_EQ(player->GetHealth(), 100);
    }

    void TearDown() override {
        GameServerBotCombatTestHarness::Reset(server_);
        connection_.reset();
        if (sender_) sender_->Close();
        sender_.reset();
        receiver_.Close();
        SocketFactory::Shutdown();
    }

    std::optional<BotHumanCombatEvent> GenerateShot() {
        BotManager* bots = server_.GetBotManager();
        const std::shared_ptr<Player> player =
            server_.GetPlayerManager()->GetPlayer(kHumanClientId);
        if (!bots || !player || !player->IsAlive()) return std::nullopt;

        if (!bots->SetExternalHumanCombatants({BotExternalHumanSnapshot{
                ParticipantId::Human(kHumanClientId),
                BotManager::kTeamTwo, player->GetPosition(), true}}) ||
            !bots->Update(0.1f)) {
            return std::nullopt;
        }
        std::vector<BotHumanCombatEvent> shots =
            bots->ConsumeHumanCombatEvents();
        if (shots.size() != 1) return std::nullopt;
        return shots.front();
    }

    std::uint16_t BindReceiver() {
        constexpr std::uint32_t kFirstPort = 30000;
        constexpr std::uint32_t kPortCount = 20000;
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint32_t start =
            static_cast<std::uint32_t>(seed % kPortCount);
        for (std::uint32_t offset = 0; offset < 2048; ++offset) {
            const auto port = static_cast<std::uint16_t>(
                kFirstPort + ((start + offset) % kPortCount));
            if (receiver_.Bind(port)) return port;
        }
        return 0;
    }

    std::vector<int> DrainHealth() {
        std::vector<int> values;
        for (int packetIndex = 0; packetIndex < 32; ++packetIndex) {
            std::string sourceIp;
            std::uint16_t sourcePort = 0;
            std::vector<std::uint8_t> bytes(2048);
            const int received = receiver_.ReceiveFrom(
                sourceIp, sourcePort, bytes.data(),
                static_cast<int>(bytes.size()));
            if (received <= 0) break;
            bytes.resize(static_cast<std::size_t>(received));
            PacketMetadata metadata{};
            Packet packet = Packet::FromBuffer(bytes, metadata);
            if (packet.GetTag() == "HEALTH_UPDATE" &&
                packet.GetPayloadSize() == sizeof(std::int32_t)) {
                values.push_back(packet.ReadInt());
            }
        }
        return values;
    }

    std::vector<int> WaitForHealth() {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(250);
        do {
            std::vector<int> values = DrainHealth();
            if (!values.empty()) {
                // Sends happen synchronously, but give loopback one bounded
                // scheduling turn before declaring that the batch contained
                // exactly one replication.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::vector<int> trailing = DrainHealth();
                values.insert(values.end(), trailing.begin(), trailing.end());
                return values;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return {};
    }

    void ExpectOnlyHealth(int expectedHealth) {
        const std::vector<int> replicated = WaitForHealth();
        ASSERT_EQ(replicated.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(replicated.front(), expectedHealth);
    }

    void ExpectNoHealth() {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(10);
        std::vector<int> replicated;
        do {
            std::vector<int> batch = DrainHealth();
            replicated.insert(
                replicated.end(), batch.begin(), batch.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        EXPECT_TRUE(replicated.empty());
    }

    GameServer server_;
    UDPSocket receiver_;
    std::uint16_t receiverPort_ = 0;
    std::shared_ptr<UDPSocket> sender_;
    std::shared_ptr<ClientConnection> connection_;
};

TEST_F(GameServerBotCombatTest,
       NorthType67UsesItsOwnAuthorityProfileWithoutInventingAVisualClass) {
    constexpr std::uint32_t kSouthM61WeaponClassRef = 286464u;
    constexpr std::uint32_t kNorthType67WeaponClassRef = 286804u;

    const auto equipped =
        server_.GetCombatAuthority()->GetParticipant(kHumanClientId);
    ASSERT_TRUE(equipped.has_value());
    EXPECT_TRUE(equipped->weapons.contains("Type67"));
    EXPECT_FALSE(equipped->weapons.contains("M61"));

    // Stable channel 212 is not enough to establish identity: the owning actor
    // class must agree with the participant's authoritative faction.
    EXPECT_FALSE(server_.SelectCombatWeaponChannel(
        kHumanClientId, 212u, kSouthM61WeaponClassRef));
    EXPECT_TRUE(server_.SelectCombatWeaponChannel(
        kHumanClientId, 212u, kNorthType67WeaponClassRef));
    EXPECT_TRUE(server_.BeginRetailGrenadeCook(
        kHumanClientId, GameplayRpc::kM61OverhandFireMode));
    EXPECT_TRUE(server_.ReleaseRetailGrenade(
        kHumanClientId, GameplayRpc::kM61OverhandFireMode,
        Vector3(1.0f, 0.0f, 0.0f)));

    const auto projectile = server_.GetCombatAuthority()->GetProjectile(1u);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_EQ(projectile->weaponId, std::string("Type67"));
    EXPECT_FLOAT_EQ(projectile->spec.fuseSeconds,
                    GameplayRpc::kType67FuseSeconds);
    EXPECT_FLOAT_EQ(projectile->spec.explosionRadiusMeters, 12.5f);
    EXPECT_FLOAT_EQ(projectile->spec.maxExplosionDamage, 200.0f);
    EXPECT_FALSE(GameServerBotCombatTestHarness::HasM61VisualProjectile(
        server_, projectile->id));
}

TEST_F(GameServerBotCombatTest,
       AcceptedAndLethalHitsReplicateAndTransitionExactlyOnce) {
    const std::optional<BotHumanCombatEvent> first = GenerateShot();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->generation, kInitialGeneration);

    // The coordinator returns true only for a lethal resolution that changed
    // bot credit; a nonlethal accepted hit is proven by authoritative state.
    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *first));
    EXPECT_FALSE(server_.GetBotManager()->IsPendingHumanCombatEvent(*first));
    const std::shared_ptr<Player> player =
        server_.GetPlayerManager()->GetPlayer(kHumanClientId);
    ASSERT_TRUE(player != nullptr);
    EXPECT_EQ(player->GetHealth(), 40);
    const auto authorityAfterFirst =
        server_.GetCombatAuthority()->GetParticipant(kHumanClientId);
    ASSERT_TRUE(authorityAfterFirst.has_value());
    EXPECT_FLOAT_EQ(authorityAfterFirst->health, 40.0f);
    EXPECT_EQ(server_.GetPlayerManager()->GetPlayerDeaths(kHumanClientId), 0);
    EXPECT_EQ(server_.GetTicketSystem()->GetTickets(BotManager::kTeamTwo), 10u);
    const BotSnapshot* attacker =
        server_.GetBotManager()->FindBot(first->attackerId);
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(0));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(0));
    ExpectOnlyHealth(40);

    // A consumed nonlethal event is inert, including at the replication edge.
    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *first));
    EXPECT_EQ(player->GetHealth(), 40);
    ExpectNoHealth();

    const std::optional<BotHumanCombatEvent> lethal = GenerateShot();
    ASSERT_TRUE(lethal.has_value());
    ASSERT_TRUE(GameServerBotCombatTestHarness::Process(server_, *lethal));
    EXPECT_FALSE(server_.GetBotManager()->IsPendingHumanCombatEvent(*lethal));
    EXPECT_EQ(player->GetHealth(), 0);
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(server_.GetPlayerManager()->GetPlayerDeaths(kHumanClientId), 1);
    EXPECT_EQ(server_.GetTicketSystem()->GetTickets(BotManager::kTeamTwo), 9u);
    const auto authorityAfterLethal =
        server_.GetCombatAuthority()->GetParticipant(kHumanClientId);
    ASSERT_TRUE(authorityAfterLethal.has_value());
    EXPECT_FALSE(authorityAfterLethal->alive);
    EXPECT_EQ(authorityAfterLethal->deaths, static_cast<std::uint32_t>(1));
    attacker = server_.GetBotManager()->FindBot(lethal->attackerId);
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(1));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(1));
    ExpectOnlyHealth(0);

    // A duplicate lethal event cannot repeat death, ticket, credit, or packet.
    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *lethal));
    EXPECT_EQ(server_.GetPlayerManager()->GetPlayerDeaths(kHumanClientId), 1);
    EXPECT_EQ(server_.GetTicketSystem()->GetTickets(BotManager::kTeamTwo), 9u);
    attacker = server_.GetBotManager()->FindBot(lethal->attackerId);
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(1));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(1));
    ExpectNoHealth();
}

TEST_F(GameServerBotCombatTest,
       DisconnectRejectsPendingShotWithoutTicketCreditOrReplication) {
    const std::optional<BotHumanCombatEvent> pending = GenerateShot();
    ASSERT_TRUE(pending.has_value());
    ASSERT_TRUE(server_.GetBotManager()->IsPendingHumanCombatEvent(*pending));

    server_.OnClientDisconnected(kHumanClientId);
    EXPECT_TRUE(server_.GetPlayerManager()->GetPlayer(kHumanClientId) == nullptr);
    EXPECT_FALSE(server_.GetCombatAuthority()
                     ->GetParticipant(kHumanClientId).has_value());
    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *pending));
    EXPECT_FALSE(server_.GetBotManager()->IsPendingHumanCombatEvent(*pending));
    EXPECT_EQ(server_.GetTicketSystem()->GetTickets(BotManager::kTeamTwo), 10u);
    const BotSnapshot* attacker =
        server_.GetBotManager()->FindBot(pending->attackerId);
    ASSERT_TRUE(attacker != nullptr);
    EXPECT_EQ(attacker->kills, static_cast<std::uint32_t>(0));
    EXPECT_EQ(attacker->score, static_cast<std::uint32_t>(0));
    ExpectNoHealth();
}

TEST_F(GameServerBotCombatTest,
       MapGenerationRejectsAliasedOldShotWithoutConsumingNewShot) {
    const std::optional<BotHumanCombatEvent> oldShot = GenerateShot();
    ASSERT_TRUE(oldShot.has_value());

    GameServerBotCombatTestHarness::ReplaceBots(
        server_, CombatConfig(kInitialGeneration + 1));
    PrepareBots(*server_.GetBotManager());
    const std::optional<BotHumanCombatEvent> newShot = GenerateShot();
    ASSERT_TRUE(newShot.has_value());

    // Fresh managers intentionally reuse bot id and shot sequence. Generation
    // is the only identity field that must differ across this map boundary.
    EXPECT_EQ(oldShot->shotSequence, newShot->shotSequence);
    EXPECT_EQ(oldShot->attackerId, newShot->attackerId);
    EXPECT_EQ(oldShot->victimId, newShot->victimId);
    EXPECT_EQ(oldShot->attackerTeamId, newShot->attackerTeamId);
    EXPECT_EQ(oldShot->origin, newShot->origin);
    EXPECT_EQ(oldShot->impact, newShot->impact);
    EXPECT_FLOAT_EQ(oldShot->distanceUu, newShot->distanceUu);
    EXPECT_FLOAT_EQ(oldShot->damage, newShot->damage);
    EXPECT_NE(oldShot->generation, newShot->generation);

    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *oldShot));
    EXPECT_TRUE(server_.GetBotManager()->IsPendingHumanCombatEvent(*newShot));
    const std::shared_ptr<Player> player =
        server_.GetPlayerManager()->GetPlayer(kHumanClientId);
    ASSERT_TRUE(player != nullptr);
    EXPECT_EQ(player->GetHealth(), 100);
    EXPECT_EQ(server_.GetTicketSystem()->GetTickets(BotManager::kTeamTwo), 10u);
    ExpectNoHealth();

    EXPECT_FALSE(GameServerBotCombatTestHarness::Process(server_, *newShot));
    EXPECT_FALSE(server_.GetBotManager()->IsPendingHumanCombatEvent(*newShot));
    EXPECT_EQ(player->GetHealth(), 40);
    ExpectOnlyHealth(40);
}

TEST(GameServerBotCombat,
     MutualLastTicketVolleyIsADrawInEitherVictimPublicationOrder) {
    const MutualVolleyResult southFirst = RunMutualLastTicketVolley(false);
    const MutualVolleyResult northFirst = RunMutualLastTicketVolley(true);

    for (const MutualVolleyResult* result : {&southFirst, &northFirst}) {
        EXPECT_TRUE(result->updated);
        EXPECT_EQ(result->batchCallbacks, static_cast<std::size_t>(1));
        ASSERT_EQ(result->batchSizes.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(result->batchSizes.front(), static_cast<std::size_t>(2));
        EXPECT_EQ(result->southTickets, 0u);
        EXPECT_EQ(result->northTickets, 0u);
        EXPECT_EQ(result->phase, SkirmishMode::Phase::PostRound);
        EXPECT_EQ(result->southWins, 0);
        EXPECT_EQ(result->northWins, 0);
    }

    ASSERT_EQ(southFirst.firstVictimTeams.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(northFirst.firstVictimTeams.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(southFirst.firstVictimTeams.front(), BotManager::kTeamOne);
    EXPECT_EQ(northFirst.firstVictimTeams.front(), BotManager::kTeamTwo);
}

TEST(GameServerBotCombat,
     SupremacyMutualLastTicketVolleyIsADrawInEitherPublicationOrder) {
    const SupremacyVolleyResult southFirst =
        RunSupremacyMutualLastTicketVolley(false);
    const SupremacyVolleyResult northFirst =
        RunSupremacyMutualLastTicketVolley(true);

    for (const SupremacyVolleyResult* result : {&southFirst, &northFirst}) {
        EXPECT_TRUE(result->updated);
        EXPECT_EQ(result->batchCallbacks, static_cast<std::size_t>(1));
        ASSERT_EQ(result->batchSizes.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(result->batchSizes.front(), static_cast<std::size_t>(2));
        EXPECT_EQ(result->southTickets, 0u);
        EXPECT_EQ(result->northTickets, 0u);
        EXPECT_EQ(result->phase, SupremacyMode::Phase::PostRound);
        EXPECT_EQ(result->winningTeam, 0u);
    }

    ASSERT_EQ(southFirst.firstVictimTeams.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(northFirst.firstVictimTeams.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(southFirst.firstVictimTeams.front(), BotManager::kTeamOne);
    EXPECT_EQ(northFirst.firstVictimTeams.front(), BotManager::kTeamTwo);
}

TEST(GameServerBotCombat,
     SupremacyMutualHumanLastTicketDeathsWaitForThePostBatchUpdate) {
    for (const bool southVictimFirst : {true, false}) {
        GameServer server;
        GameServerBotCombatTestHarness::InstallSupremacyHumanTicketVolley(
            server);
        SupremacyMode* mode = server.GetSupremacyMode();
        mode->StartRound();
        mode->Update(30.0f);
        ASSERT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);

        CombatAuthority::CombatEvent southDeath;
        southDeath.kind = CombatAuthority::EventKind::ParticipantDied;
        southDeath.sourceId = 202u;
        southDeath.targetId = 101u;
        CombatAuthority::CombatEvent northDeath;
        northDeath.kind = CombatAuthority::EventKind::ParticipantDied;
        northDeath.sourceId = 101u;
        northDeath.targetId = 202u;

        const std::vector<CombatAuthority::CombatEvent> deaths =
            southVictimFirst
                ? std::vector<CombatAuthority::CombatEvent>{southDeath,
                                                            northDeath}
                : std::vector<CombatAuthority::CombatEvent>{northDeath,
                                                            southDeath};
        GameServerBotCombatTestHarness::ProcessCombatEvents(server, deaths);

        EXPECT_EQ(server.GetTicketSystem()->GetTickets(
                      BotManager::kTeamOne),
                  0u);
        EXPECT_EQ(server.GetTicketSystem()->GetTickets(
                      BotManager::kTeamTwo),
                  0u);
        EXPECT_FALSE(server.GetPlayerManager()->GetPlayer(101u)->IsAlive());
        EXPECT_FALSE(server.GetPlayerManager()->GetPlayer(202u)->IsAlive());
        // The old per-death resolution awarded the still-alive opponent here,
        // making this assertion fail in one direction or the other.
        EXPECT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);
        EXPECT_EQ(mode->GetWinningTeam(), 0u);

        mode->Update(0.0f);
        EXPECT_EQ(mode->GetPhase(), SupremacyMode::Phase::PostRound);
        EXPECT_EQ(mode->GetWinningTeam(), 0u);

        GameServerBotCombatTestHarness::Reset(server);
    }
}

TEST(GameServerBotCombat,
     SupremacySecondRoundRestoresConfiguredPoolsAndUnlimitedSentinel) {
    GameServer server;
    GameServerBotCombatTestHarness::InstallSupremacyTicketLifecycle(
        server, 3u, 0u);
    SupremacyMode* mode = server.GetSupremacyMode();
    TicketSystem* tickets = server.GetTicketSystem();
    mode->StartRound();
    mode->Update(30.0f);
    ASSERT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);

    tickets->ConsumeTicket(BotManager::kTeamOne, 3u);
    tickets->AddTickets(BotManager::kTeamOne, 1u);
    mode->Update(0.0f);
    EXPECT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);
    EXPECT_EQ(tickets->GetTickets(BotManager::kTeamOne), 1u);

    tickets->ConsumeTicket(BotManager::kTeamOne);
    mode->Update(0.0f);
    ASSERT_EQ(mode->GetPhase(), SupremacyMode::Phase::PostRound);
    EXPECT_EQ(mode->GetWinningTeam(), SupremacyMode::kNorthTeamId);

    mode->StartRound();
    EXPECT_EQ(mode->GetPhase(), SupremacyMode::Phase::Preparation);
    EXPECT_EQ(tickets->GetTickets(BotManager::kTeamOne), 3u);
    EXPECT_EQ(tickets->GetTickets(BotManager::kTeamTwo), 0u);
    EXPECT_EQ(tickets->GetInitialTickets(BotManager::kTeamOne), 3u);
    EXPECT_EQ(tickets->GetInitialTickets(BotManager::kTeamTwo), 0u);
    EXPECT_TRUE(tickets->HasTickets(BotManager::kTeamTwo));

    mode->Update(30.0f);
    ASSERT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);
    tickets->SetTickets(BotManager::kTeamOne, 0u);
    tickets->AddTickets(BotManager::kTeamOne, 1u);
    mode->Update(0.0f);
    EXPECT_EQ(mode->GetPhase(), SupremacyMode::Phase::Active);
    EXPECT_EQ(tickets->GetTickets(BotManager::kTeamOne), 1u);

    GameServerBotCombatTestHarness::Reset(server);
}

TEST(GameServerBotCombat,
     SupremacyBotRespawnGateClosesOnlyTheDepletedTeam) {
    BotManagerConfig config = CombatConfig(kInitialGeneration + 300);
    GameServer server;
    GameServerBotCombatTestHarness::InstallSupremacyRespawnGate(server, config);

    GameServerBotCombatTestHarness::RefreshBotWorldState(server);
    EXPECT_EQ(server.GetBotManager()->CountAliveBots(BotManager::kTeamOne),
              static_cast<std::size_t>(0));
    EXPECT_EQ(server.GetBotManager()->CountAliveBots(BotManager::kTeamTwo),
              static_cast<std::size_t>(1));

    server.GetTicketSystem()->SetTickets(BotManager::kTeamOne, 1);
    GameServerBotCombatTestHarness::RefreshBotWorldState(server);
    EXPECT_EQ(server.GetBotManager()->CountAliveBots(BotManager::kTeamOne),
              static_cast<std::size_t>(1));
    EXPECT_EQ(server.GetBotManager()->CountAliveBots(BotManager::kTeamTwo),
              static_cast<std::size_t>(1));

    GameServerBotCombatTestHarness::Reset(server);
}

} // namespace

RS2V_TEST_MAIN()
