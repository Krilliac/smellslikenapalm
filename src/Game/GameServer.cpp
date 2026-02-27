// src/Game/GameServer.cpp

#include "Game/GameServer.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Game/AdminManager.h"
#include "Game/ChatManager.h"
#include "Game/GameMode.h"
#include "Config/ConfigManager.h"
#include "Config/GameConfig.h"
#include "Config/NetworkConfig.h"
#include "Config/ServerConfig.h"
#include "Config/SecurityConfig.h"
#include "Config/MapConfig.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Game/RoleSystem.h"
#include "Game/TicketSystem.h"
#include "Game/ObjectiveSystem.h"
#include "Game/CommanderAbilities.h"
#include "Game/SpawnSystem.h"
#include "Game/WeaponDatabase.h"
#include "Game/DamageSystem.h"
#include "Game/HelicopterPhysics.h"
#include "Game/TerritoryMode.h"
#include "Game/SupremacyMode.h"
#include "Game/SkirmishMode.h"
#include "Utils/PathUtils.h"
#include "Utils/HandlerLibraryManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using namespace GeneratedHandlers;

// --- Handler Regen/Reload Implementation ---

void GameServer::Cmd_RegenHandlers(const std::vector<std::string>& /*args*/) {
    std::string codeGenExe = PathUtils::ResolveFromExecutable("PacketHandlerCodeGen");
#ifdef _WIN32
    if (codeGenExe.find(".exe") == std::string::npos) codeGenExe += ".exe";
#endif
    std::string handlersDir = PathUtils::ResolveFromExecutable("src/Generated/Handlers");
    std::string cmd = codeGenExe + " " + handlersDir;

    Logger::Info("Executing handler regeneration: %s", cmd.c_str());
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        Logger::Info("Handler stubs regenerated successfully.");
        DynamicReloadGeneratedHandlers();
    } else {
        Logger::Error("Failed to regenerate handler stubs (exit %d)", ret);
    }
}

void GameServer::StartAutoRegen(int intervalSeconds) {
    if (m_regenRunning) return;
    m_regenRunning = true;
    m_regenIntervalSeconds = intervalSeconds;
    m_regenThread = std::thread([this]() {
        Logger::Info("Auto handler regeneration thread started (interval: %d seconds)", m_regenIntervalSeconds);
        while (m_regenRunning) {
            // Sleep for the configured interval, checking for shutdown each second
            for (int i = 0; i < m_regenIntervalSeconds && m_regenRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (m_regenRunning) {
                Cmd_RegenHandlers();
            }
        }
        Logger::Info("Auto handler regeneration thread stopped");
    });
    m_regenThread.detach();
}

void GameServer::StopAutoRegen() {
    m_regenRunning = false;
    Logger::Info("Auto handler regeneration requested to stop.");
}

void GameServer::DynamicReloadGeneratedHandlers() {
    if (m_handlerLibraryPath.empty()) {
#ifdef _WIN32
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("GeneratedHandlers.dll");
#elif defined(__APPLE__)
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.dylib");
#else
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.so");
#endif
        if (!std::filesystem::exists(m_handlerLibraryPath)) {
            Logger::Warn("Handler library not found: %s", m_handlerLibraryPath.c_str());
            return;
        }
        if (!HandlerLibraryManager::Instance().Initialize(m_handlerLibraryPath)) {
            Logger::Error("Failed to initialize handler library manager");
            return;
        }
    }
    if (HandlerLibraryManager::Instance().ForceReload()) {
        Logger::Info("Generated handlers reloaded successfully.");
    } else {
        Logger::Error("Failed to reload generated handlers.");
    }
}

// --- Core GameServer Implementation ---

GameServer::GameServer() {
    Logger::Info("GameServer constructed");
}

GameServer::~GameServer() {
    Shutdown();
    StopAutoRegen();
    GetProtocolDecoder().Shutdown();
    HandlerLibraryManager::Instance().Shutdown();
}

bool GameServer::Initialize() {
    Logger::Info("GameServer initialization starting...");

    // Load configurations
    m_configManager = std::make_shared<ConfigManager>();
    if (!m_configManager->Initialize()) {
        Logger::Warn("ConfigManager init failed — using defaults");
    }

    // Create config wrappers — ServerConfig wraps ConfigManager directly,
    // the rest wrap ServerConfig
    m_serverConfig   = std::make_shared<ServerConfig>(m_configManager);
    m_networkConfig  = std::make_shared<NetworkConfig>(*m_serverConfig);
    m_securityConfig = std::make_shared<SecurityConfig>(*m_serverConfig);
    m_gameConfig     = std::make_shared<GameConfig>(*m_serverConfig);
    m_mapConfig      = std::make_shared<MapConfig>(*m_serverConfig);

    // Initialize network manager
    m_networkManager = std::make_unique<NetworkManager>(this);
    uint16_t listenPort = (uint16_t)m_serverConfig->GetPort();
    if (!m_networkManager->Initialize(listenPort)) {
        Logger::Error("NetworkManager init failed on port %u", listenPort);
        return false;
    }

    // Initialize game managers
    m_playerManager = std::make_unique<PlayerManager>(this);
    m_teamManager   = std::make_unique<TeamManager>(this);
    m_mapManager    = std::make_unique<MapManager>(this, m_mapConfig);

    m_adminManager  = std::make_unique<AdminManager>(this, m_serverConfig);
    m_adminManager->Initialize();

    m_chatManager   = std::make_unique<ChatManager>(this);
    m_chatManager->Initialize();

    // Initialize RS2V game systems
    m_weaponDatabase = std::make_unique<WeaponDatabase>();
    m_weaponDatabase->Initialize();

    m_roleSystem = std::make_unique<RoleSystem>(this);
    m_roleSystem->Initialize();
    m_roleSystem->SetTeamFaction(1, Faction::USArmy);
    m_roleSystem->SetTeamFaction(2, Faction::NVA);

    m_ticketSystem = std::make_unique<TicketSystem>(this);
    m_ticketSystem->Initialize(300, 300);
    m_ticketSystem->SetOnTicketsDepleted([this](uint32_t teamId) {
        Logger::Info("Team %u tickets depleted", teamId);
        if (m_territoryMode) m_territoryMode->OnTicketsDepleted(teamId);
        if (m_supremacyMode) m_supremacyMode->OnTicketsDepleted(teamId);
        if (m_skirmishMode) m_skirmishMode->OnTicketsDepleted(teamId);
    });

    m_objectiveSystem = std::make_unique<ObjectiveSystem>(this);
    m_objectiveSystem->Initialize();
    m_objectiveSystem->SetOnObjectiveCaptured([this](uint32_t objId, uint32_t capTeam, uint32_t prevTeam) {
        Logger::Info("Objective %u captured by team %u (was team %u)", objId, capTeam, prevTeam);
        if (m_territoryMode) m_territoryMode->OnObjectiveCaptured(objId, capTeam);
        if (m_supremacyMode) m_supremacyMode->OnObjectiveCaptured(objId, capTeam);
        if (m_skirmishMode) m_skirmishMode->OnObjectiveCaptured(objId, capTeam);
    });

    m_commanderAbilities = std::make_unique<CommanderAbilities>(this);
    m_commanderAbilities->Initialize();

    m_spawnSystem = std::make_unique<SpawnSystem>(this);
    m_spawnSystem->Initialize();

    m_damageSystem = std::make_unique<DamageSystem>(this);
    m_damageSystem->Initialize();
    m_damageSystem->SetFriendlyFireEnabled(m_gameConfig->IsFriendlyFire());
    m_damageSystem->SetOnKill([this](const KillEvent& kill) {
        if (m_ticketSystem && !kill.isTeamKill) {
            auto* tm = GetTeamManager();
            uint32_t victimTeam = tm->GetPlayerTeam(kill.victimId);
            m_ticketSystem->OnPlayerKilled(victimTeam);
        }
        if (m_territoryMode) m_territoryMode->OnPlayerKilled(kill.killerId, kill.victimId);
        if (m_supremacyMode) m_supremacyMode->OnPlayerKilled(kill.killerId, kill.victimId);
        if (m_skirmishMode) m_skirmishMode->OnPlayerKilled(kill.killerId, kill.victimId);
    });

    m_helicopterPhysics = std::make_unique<HelicopterPhysics>(this);
    m_helicopterPhysics->Initialize();

    m_territoryMode = std::make_unique<TerritoryMode>(this);
    m_territoryMode->Initialize();

    m_supremacyMode = std::make_unique<SupremacyMode>(this);
    m_supremacyMode->Initialize();

    m_skirmishMode = std::make_unique<SkirmishMode>(this);
    m_skirmishMode->Initialize();

    Logger::Info("RS2V game systems initialized");

    // Start first map and game mode
    std::string mapName = m_gameConfig->GetGameSettings().mapName;
    if (!mapName.empty() && !m_mapManager->LoadMap(mapName)) {
        Logger::Warn("Failed to load map: %s — continuing without map", mapName.c_str());
    }

    const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
    if (gmDef) {
        m_gameMode = std::make_unique<GameMode>(this, *gmDef);
        m_gameMode->OnStart();
    } else {
        Logger::Warn("No valid game mode definition found — continuing without game mode");
    }

    // Initialize protocol reverse-engineering decoder
    {
        ProtocolDecoderConfig decoderCfg;
        decoderCfg.enabled = m_configManager->GetBool("ReverseEngineering.enabled", true);
        decoderCfg.logRawPackets = m_configManager->GetBool("ReverseEngineering.log_raw_packets", true);
        decoderCfg.exportJsonDefinitions = m_configManager->GetBool("ReverseEngineering.export_json", true);
        decoderCfg.detectUE3Bunches = m_configManager->GetBool("ReverseEngineering.detect_ue3_bunches", true);
        decoderCfg.outputDirectory = m_configManager->GetString("ReverseEngineering.output_dir", "protocol_analysis");
        decoderCfg.exportIntervalSeconds = m_configManager->GetInt("ReverseEngineering.export_interval", 300);
        GetProtocolDecoder().Initialize(decoderCfg);

        GetProtocolDecoder().SetDiscoveryCallback([](const std::string& tag, const DecodedPacketStructure& s) {
            Logger::Info("[RE] New packet type discovered: '%s' (payload=%zu bytes)",
                         tag.c_str(), s.avgPayloadSize > 0 ? (size_t)s.avgPayloadSize : 0);
        });
    }

    // Start periodic handler regeneration (every 1 hour by default)
    StartAutoRegen(3600);

    m_running = true;
    Logger::Info("GameServer initialized successfully");
    return true;
}

void GameServer::Run() {
    if (!m_running) return;

    // Poll network and process received packets
    if (m_networkManager) {
        m_networkManager->PollNetwork();
    }

    // Process queued packets
    auto packets = FetchPendingPackets();
    for (auto& qpkt : packets) {
        auto conn = GetClientConnection(qpkt.clientId);
        if (!conn) continue;

        std::string tag = qpkt.packet.GetTag();
        if (tag == "CHAT_MESSAGE" && m_chatManager) {
            Packet pktCopy = qpkt.packet;
            std::string chatText = pktCopy.ReadString();
            m_chatManager->ProcessChatCommand(qpkt.clientId, chatText);
        } else if (tag == "ROLE_SELECT") {
            HandleRoleSelection(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "SPAWN_REQUEST") {
            HandleSpawnRequest(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "COMMANDER_ABILITY") {
            HandleCommanderAbility(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "SQUAD_ACTION") {
            HandleSquadAction(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "VEHICLE_ACTION") {
            HandleVehicleAction(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "WEAPON_FIRE") {
            HandleWeaponFire(qpkt.clientId, qpkt.packet.RawData());
        } else if (m_gameMode) {
            m_gameMode->HandlePlayerAction(qpkt.clientId, tag, qpkt.packet.RawData());
        }
    }

    float dt = m_tickDeltaSeconds;

    // Tick core subsystems
    if (m_gameMode) m_gameMode->Update();
    if (m_playerManager) m_playerManager->Update();

    // Tick RS2V game systems
    if (m_ticketSystem) m_ticketSystem->Update(dt);
    if (m_objectiveSystem) m_objectiveSystem->Update(dt);
    if (m_commanderAbilities) m_commanderAbilities->Update(dt);
    if (m_spawnSystem) m_spawnSystem->Update(dt);
    if (m_damageSystem) m_damageSystem->Update(dt);
    if (m_helicopterPhysics) m_helicopterPhysics->Update(dt);

    // Tick active game mode
    if (m_territoryMode) m_territoryMode->Update(dt);
    if (m_supremacyMode) m_supremacyMode->Update(dt);
    if (m_skirmishMode) m_skirmishMode->Update(dt);

    if (m_networkManager) m_networkManager->Flush();
}

void GameServer::Shutdown() {
    if (!m_running) return;
    Logger::Info("Shutting down GameServer...");

    m_running = false;
    StopAutoRegen();

    // Shutdown RS2V systems
    m_skirmishMode.reset();
    m_supremacyMode.reset();
    m_territoryMode.reset();
    m_helicopterPhysics.reset();
    m_damageSystem.reset();
    m_spawnSystem.reset();
    m_commanderAbilities.reset();
    m_objectiveSystem.reset();
    m_ticketSystem.reset();
    m_roleSystem.reset();
    m_weaponDatabase.reset();

    if (m_gameMode) {
        m_gameMode->OnEnd();
        m_gameMode.reset();
    }
    m_chatManager.reset();
    if (m_adminManager) {
        m_adminManager->Shutdown();
        m_adminManager.reset();
    }
    m_mapManager.reset();
    m_teamManager.reset();
    m_playerManager.reset();
    if (m_networkManager) {
        m_networkManager->Shutdown();
        m_networkManager.reset();
    }

    Logger::Info("GameServer shutdown complete");
}

void GameServer::BroadcastChatMessage(const std::string& msg) {
    if (m_chatManager) m_chatManager->BroadcastChat(msg);
}

uint32_t GameServer::FindClientBySteamID(const std::string& steamId) const {
    return m_networkManager ? m_networkManager->FindClientBySteamID(steamId) : UINT32_MAX;
}

std::shared_ptr<ClientConnection> GameServer::GetClientConnection(uint32_t clientId) const {
    return m_networkManager ? m_networkManager->GetConnection(clientId) : nullptr;
}

std::vector<std::shared_ptr<ClientConnection>> GameServer::GetAllConnections() const {
    return m_networkManager ? m_networkManager->GetAllConnections() : std::vector<std::shared_ptr<ClientConnection>>{};
}

// Subsystem accessors
PlayerManager*      GameServer::GetPlayerManager()      const { return m_playerManager.get();      }
TeamManager*        GameServer::GetTeamManager()        const { return m_teamManager.get();        }
MapManager*         GameServer::GetMapManager()         const { return m_mapManager.get();         }
NetworkManager*     GameServer::GetNetworkManager()     const { return m_networkManager.get();     }
AdminManager*       GameServer::GetAdminManager()       const { return m_adminManager.get();       }
RoleSystem*         GameServer::GetRoleSystem()         const { return m_roleSystem.get();         }
TicketSystem*       GameServer::GetTicketSystem()       const { return m_ticketSystem.get();       }
ObjectiveSystem*    GameServer::GetObjectiveSystem()    const { return m_objectiveSystem.get();    }
CommanderAbilities* GameServer::GetCommanderAbilities() const { return m_commanderAbilities.get(); }
SpawnSystem*        GameServer::GetSpawnSystem()        const { return m_spawnSystem.get();        }
WeaponDatabase*     GameServer::GetWeaponDatabase()     const { return m_weaponDatabase.get();     }
DamageSystem*       GameServer::GetDamageSystem()       const { return m_damageSystem.get();       }
HelicopterPhysics*  GameServer::GetHelicopterPhysics()  const { return m_helicopterPhysics.get();  }
TerritoryMode*      GameServer::GetTerritoryMode()      const { return m_territoryMode.get();      }
SupremacyMode*      GameServer::GetSupremacyMode()      const { return m_supremacyMode.get();      }
SkirmishMode*       GameServer::GetSkirmishMode()       const { return m_skirmishMode.get();       }

std::shared_ptr<GameConfig>    GameServer::GetGameConfig()    const { return m_gameConfig;    }
std::shared_ptr<ServerConfig>  GameServer::GetServerConfig()  const { return m_serverConfig;  }
std::shared_ptr<ConfigManager> GameServer::GetConfigManager() const { return m_configManager; }

void GameServer::ChangeMap() {
    std::string nextMap = m_mapManager->GetNextMap();
    if (m_mapManager->LoadMap(nextMap)) {
        if (m_gameMode) m_gameMode->OnEnd();
        const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
        if (gmDef) {
            m_gameMode = std::make_unique<GameMode>(this, *gmDef);
            m_gameMode->OnStart();
        }
    } else {
        Logger::Error("Failed to change to map: %s", nextMap.c_str());
    }
}

// Packet queue implementation
void GameServer::EnqueuePacket(const QueuedPacket& pkt) {
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    m_packetQueue.push(pkt);
}

std::vector<QueuedPacket> GameServer::FetchPendingPackets() {
    std::vector<QueuedPacket> out;
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    while (!m_packetQueue.empty()) {
        out.push_back(std::move(m_packetQueue.front()));
        m_packetQueue.pop();
    }
    return out;
}

void GameServer::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    EnqueuePacket({clientId, pkt, meta});
}

void GameServer::ProcessNetworkMessages() {
    if (m_networkManager) {
        m_networkManager->PollNetwork();
    }
}

std::string GameServer::GetExeDir() const {
    return PathUtils::GetExecutableDirectory();
}

// ============================================================================
// RS2V Packet Handlers
// ============================================================================

void GameServer::HandleRoleSelection(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_roleSystem || data.size() < 1) return;

    CombatRole role = static_cast<CombatRole>(data[0]);
    if (m_roleSystem->AssignRole(clientId, role)) {
        Logger::Info("Player %u selected role: %s", clientId, m_roleSystem->GetRoleName(role).c_str());

        // Apply role loadout to player
        auto* tm = GetTeamManager();
        Faction faction = m_roleSystem->GetTeamFaction(tm->GetPlayerTeam(clientId));
        RoleLoadout loadout = m_roleSystem->GetRoleLoadout(role, faction);

        auto* pm = GetPlayerManager();
        auto player = pm->GetPlayer(clientId);
        if (player) {
            player->ClearInventory();
            player->AddItem(loadout.primaryWeapon, loadout.primaryAmmo);
            if (!loadout.secondaryWeapon.empty()) {
                player->AddItem(loadout.secondaryWeapon, loadout.secondaryAmmo);
            }
            for (const auto& eq : loadout.equipment) {
                player->AddItem(eq, 1);
            }
        }
    } else {
        Logger::Warn("Player %u role selection failed: role %d", clientId, static_cast<int>(role));
    }
}

void GameServer::HandleSpawnRequest(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_spawnSystem) return;

    if (data.size() >= 4) {
        uint32_t spawnLocId = 0;
        memcpy(&spawnLocId, data.data(), sizeof(uint32_t));
        if (m_spawnSystem->SpawnPlayer(clientId, spawnLocId)) {
            Logger::Debug("Player %u spawned at location %u", clientId, spawnLocId);
        } else {
            // Try default spawn
            m_spawnSystem->SpawnPlayerAtDefault(clientId);
        }
    } else {
        m_spawnSystem->SpawnPlayerAtDefault(clientId);
    }
}

void GameServer::HandleCommanderAbility(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_commanderAbilities || !m_roleSystem) return;

    // Verify player is commander
    if (m_roleSystem->GetPlayerRole(clientId) != CombatRole::Commander) {
        Logger::Warn("Player %u tried commander ability but is not commander", clientId);
        return;
    }

    if (data.size() < 13) return;  // 1 byte type + 12 bytes position

    AbilityType type = static_cast<AbilityType>(data[0]);
    Vector3 target;
    memcpy(&target.x, data.data() + 1, sizeof(float));
    memcpy(&target.y, data.data() + 5, sizeof(float));
    memcpy(&target.z, data.data() + 9, sizeof(float));

    Vector3 direction;
    if (data.size() >= 25) {
        memcpy(&direction.x, data.data() + 13, sizeof(float));
        memcpy(&direction.y, data.data() + 17, sizeof(float));
        memcpy(&direction.z, data.data() + 21, sizeof(float));
    }

    if (m_commanderAbilities->RequestAbility(clientId, type, target, direction)) {
        Logger::Info("Commander %u called %s at (%.1f, %.1f, %.1f)",
                     clientId, m_commanderAbilities->GetAbilityName(type).c_str(),
                     target.x, target.y, target.z);
    }
}

void GameServer::HandleSquadAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_roleSystem || data.size() < 1) return;

    uint8_t action = data[0];
    switch (action) {
        case 0: {  // Create squad
            uint32_t teamId = GetTeamManager()->GetPlayerTeam(clientId);
            uint32_t squadId = m_roleSystem->CreateSquad(teamId);
            if (squadId > 0) {
                m_roleSystem->JoinSquad(clientId, squadId);
                Logger::Info("Player %u created and joined squad %u", clientId, squadId);
            }
            break;
        }
        case 1: {  // Join squad
            if (data.size() >= 5) {
                uint32_t squadId = 0;
                memcpy(&squadId, data.data() + 1, sizeof(uint32_t));
                m_roleSystem->JoinSquad(clientId, squadId);
            }
            break;
        }
        case 2:  // Leave squad
            m_roleSystem->LeaveSquad(clientId);
            break;
        case 3:  // Volunteer as commander
            m_roleSystem->VolunteerAsCommander(clientId);
            break;
        case 4:  // Resign as commander
            m_roleSystem->ResignAsCommander(clientId);
            break;
        default:
            break;
    }
}

void GameServer::HandleVehicleAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_helicopterPhysics || data.size() < 1) return;

    uint8_t action = data[0];
    switch (action) {
        case 0: {  // Enter helicopter
            if (data.size() >= 9) {
                uint32_t heliId = 0, seat = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                memcpy(&seat, data.data() + 5, sizeof(uint32_t));
                m_helicopterPhysics->EnterHelicopter(clientId, heliId, seat);
            }
            break;
        }
        case 1:  // Exit helicopter
            m_helicopterPhysics->ExitHelicopter(clientId);
            break;
        case 2: {  // Control input
            if (data.size() >= 18) {
                HeliControlInput input;
                memcpy(&input.collective, data.data() + 1, sizeof(float));
                memcpy(&input.cyclic_pitch, data.data() + 5, sizeof(float));
                memcpy(&input.cyclic_roll, data.data() + 9, sizeof(float));
                memcpy(&input.pedal, data.data() + 13, sizeof(float));
                input.fireWeapon = (data.size() > 17 && data[17] != 0);
                // Find helicopter this player is piloting
                for (auto* h : m_helicopterPhysics->GetAllHelicopters()) {
                    if (h->pilotId == clientId) {
                        m_helicopterPhysics->SetControlInput(h->vehicleId, input);
                        break;
                    }
                }
            }
            break;
        }
        case 3: {  // Start engine
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                m_helicopterPhysics->StartEngine(heliId);
            }
            break;
        }
        case 4: {  // Stop engine
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                m_helicopterPhysics->StopEngine(heliId);
            }
            break;
        }
        default:
            break;
    }
}

void GameServer::HandleWeaponFire(uint32_t clientId, const std::vector<uint8_t>& data) {
    if (!m_damageSystem || data.size() < 29) return;

    // Parse weapon fire data: weaponId(variable) + origin(12) + direction(12) + hitZone(1) + victimId(4)
    // Simplified: first byte = weapon id string length
    uint8_t weaponIdLen = data[0];
    if (data.size() < static_cast<size_t>(1 + weaponIdLen + 29)) return;

    std::string weaponId(data.begin() + 1, data.begin() + 1 + weaponIdLen);
    size_t offset = 1 + weaponIdLen;

    Vector3 origin, direction;
    memcpy(&origin.x, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&origin.y, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&origin.z, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.x, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.y, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.z, data.data() + offset, sizeof(float)); offset += 4;

    HitZone hitZone = static_cast<HitZone>(data[offset]); offset++;
    uint32_t victimId = 0;
    memcpy(&victimId, data.data() + offset, sizeof(uint32_t));

    if (victimId == 0) return;  // Miss — no damage

    // Calculate damage using weapon database
    float distance = origin.Distance(m_playerManager->GetPlayer(victimId) ?
                     m_playerManager->GetPlayer(victimId)->GetPosition() : origin);

    auto* weaponDef = m_weaponDatabase ? m_weaponDatabase->GetWeapon(weaponId) : nullptr;
    float baseDmg = weaponDef ? m_weaponDatabase->CalculateDamage(
        weaponId, distance, hitZone == HitZone::Head,
        hitZone == HitZone::LeftArm || hitZone == HitZone::RightArm ||
        hitZone == HitZone::LeftLeg || hitZone == HitZone::RightLeg) : 50.0f;

    DamageEvent event;
    event.attackerId = clientId;
    event.victimId = victimId;
    event.source = DamageSource::Bullet;
    event.weaponId = weaponId;
    event.hitZone = hitZone;
    event.baseDamage = baseDmg;
    event.distance = distance;
    event.hitPosition = origin;
    event.hitDirection = direction;

    m_damageSystem->ApplyDamage(event);
}
