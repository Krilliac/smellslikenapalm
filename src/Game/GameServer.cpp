// src/Game/GameServer.cpp

#include "Game/GameServer.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Game/AdminManager.h"
#include "Game/ChatManager.h"
#include "Game/GameMode.h"
#include "Config/ConfigManager.h"
#include "Config/GameConfig.h"
#include "../../telemetry/TelemetryManager.h"
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
    Logger::Trace("[GameServer::Cmd_RegenHandlers] Entry");
    std::string codeGenExe = PathUtils::ResolveFromExecutable("PacketHandlerCodeGen");
#ifdef _WIN32
    if (codeGenExe.find(".exe") == std::string::npos) codeGenExe += ".exe";
#endif
    std::string handlersDir = PathUtils::ResolveFromExecutable("src/Generated/Handlers");
    std::string cmd = codeGenExe + " " + handlersDir;

    Logger::Info("[GameServer::Cmd_RegenHandlers] Executing handler regeneration: %s", cmd.c_str());
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        Logger::Debug("[GameServer::Cmd_RegenHandlers] System command succeeded, reloading handlers");
        Logger::Info("Handler stubs regenerated successfully.");
        DynamicReloadGeneratedHandlers();
    } else {
        Logger::Error("[GameServer::Cmd_RegenHandlers] Failed to regenerate handler stubs (exit %d)", ret);
    }
    Logger::Trace("[GameServer::Cmd_RegenHandlers] Exit");
}

void GameServer::StartAutoRegen(int intervalSeconds) {
    Logger::Trace("[GameServer::StartAutoRegen] Entry, intervalSeconds=%d", intervalSeconds);
    if (m_regenRunning) {
        Logger::Debug("[GameServer::StartAutoRegen] Already running, skipping");
        Logger::Trace("[GameServer::StartAutoRegen] Exit (already running)");
        return;
    }
    m_regenRunning = true;
    m_regenIntervalSeconds = intervalSeconds;
    Logger::Debug("[GameServer::StartAutoRegen] Starting regen thread with interval %d seconds", intervalSeconds);
    m_regenThread = std::thread([this]() {
        Logger::Info("Auto handler regeneration thread started (interval: %d seconds)", m_regenIntervalSeconds);
        while (m_regenRunning) {
            // Sleep for the configured interval, checking for shutdown each second
            for (int i = 0; i < m_regenIntervalSeconds && m_regenRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (m_regenRunning) {
                Logger::Debug("[GameServer::StartAutoRegen] Triggering periodic handler regeneration");
                Cmd_RegenHandlers();
            }
        }
        Logger::Info("Auto handler regeneration thread stopped");
    });
    m_regenThread.detach();
    Logger::Trace("[GameServer::StartAutoRegen] Exit");
}

void GameServer::StopAutoRegen() {
    Logger::Trace("[GameServer::StopAutoRegen] Entry");
    m_regenRunning = false;
    Logger::Info("Auto handler regeneration requested to stop.");
    Logger::Trace("[GameServer::StopAutoRegen] Exit");
}

void GameServer::DynamicReloadGeneratedHandlers() {
    Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Entry");
    if (m_handlerLibraryPath.empty()) {
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Library path empty, resolving platform-specific path");
#ifdef _WIN32
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("GeneratedHandlers.dll");
#elif defined(__APPLE__)
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.dylib");
#else
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.so");
#endif
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Resolved library path: %s", m_handlerLibraryPath.c_str());
        if (!std::filesystem::exists(m_handlerLibraryPath)) {
            Logger::Warn("Handler library not found: %s", m_handlerLibraryPath.c_str());
            Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit (library not found)");
            return;
        }
        if (!HandlerLibraryManager::Instance().Initialize(m_handlerLibraryPath)) {
            Logger::Error("[GameServer::DynamicReloadGeneratedHandlers] Failed to initialize handler library manager");
            Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit (init failed)");
            return;
        }
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Handler library manager initialized");
    }
    if (HandlerLibraryManager::Instance().ForceReload()) {
        Logger::Info("Generated handlers reloaded successfully.");
    } else {
        Logger::Error("[GameServer::DynamicReloadGeneratedHandlers] Failed to reload generated handlers.");
    }
    Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit");
}

// --- Core GameServer Implementation ---

GameServer::GameServer() {
    Logger::Trace("[GameServer::GameServer] Entry");
    Logger::Info("GameServer constructed");
    Logger::Trace("[GameServer::GameServer] Exit");
}

GameServer::~GameServer() {
    Logger::Trace("[GameServer::~GameServer] Entry");
    Shutdown();
    StopAutoRegen();
    GetProtocolDecoder().Shutdown();
    HandlerLibraryManager::Instance().Shutdown();
    Logger::Trace("[GameServer::~GameServer] Exit");
}

bool GameServer::Initialize() {
    Logger::Trace("[GameServer::Initialize] Entry");
    Logger::Info("GameServer initialization starting...");

    // Load configurations
    Logger::Debug("[GameServer::Initialize] Creating ConfigManager");
    m_configManager = std::make_shared<ConfigManager>();
    if (!m_configManager->Initialize()) {
        Logger::Warn("[GameServer::Initialize] ConfigManager init failed — using defaults");
    } else {
        Logger::Debug("[GameServer::Initialize] ConfigManager initialized successfully");
    }

    // Create config wrappers — ServerConfig wraps ConfigManager directly,
    // the rest wrap ServerConfig
    Logger::Debug("[GameServer::Initialize] Creating config wrappers");
    m_serverConfig   = std::make_shared<ServerConfig>(m_configManager);
    m_networkConfig  = std::make_shared<NetworkConfig>(*m_serverConfig);
    m_securityConfig = std::make_shared<SecurityConfig>(*m_serverConfig);
    m_gameConfig     = std::make_shared<GameConfig>(*m_serverConfig);
    m_mapConfig      = std::make_shared<MapConfig>(*m_serverConfig);

    // Initialize network manager
    m_networkManager = std::make_unique<NetworkManager>(this);
    uint16_t listenPort = (uint16_t)m_serverConfig->GetPort();
    Logger::Debug("[GameServer::Initialize] Initializing NetworkManager on port %u", listenPort);
    if (!m_networkManager->Initialize(listenPort)) {
        Logger::Error("[GameServer::Initialize] NetworkManager init failed on port %u", listenPort);
        Logger::Trace("[GameServer::Initialize] Exit, returning false");
        return false;
    }
    Logger::Debug("[GameServer::Initialize] NetworkManager initialized successfully");

    // Initialize game managers
    Logger::Debug("[GameServer::Initialize] Creating game managers");
    m_playerManager = std::make_unique<PlayerManager>(this);
    m_teamManager   = std::make_unique<TeamManager>(this);
    m_mapManager    = std::make_unique<MapManager>(this, m_mapConfig);

    m_adminManager  = std::make_unique<AdminManager>(this, m_serverConfig);
    m_adminManager->Initialize();

    m_chatManager   = std::make_unique<ChatManager>(this);
    m_chatManager->Initialize();

    // Initialize RS2V game systems
    Logger::Debug("[GameServer::Initialize] Initializing RS2V game systems");
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
    Logger::Debug("[GameServer::Initialize] Attempting to load initial map: '%s'", mapName.c_str());
    if (!mapName.empty() && !m_mapManager->LoadMap(mapName)) {
        Logger::Warn("[GameServer::Initialize] Failed to load map: %s — continuing without map", mapName.c_str());
    } else if (!mapName.empty()) {
        Logger::Debug("[GameServer::Initialize] Map '%s' loaded successfully", mapName.c_str());
    }

    const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
    if (gmDef) {
        Logger::Debug("[GameServer::Initialize] Creating GameMode from definition");
        m_gameMode = std::make_unique<GameMode>(this, *gmDef);
        m_gameMode->OnStart();
    } else {
        Logger::Warn("[GameServer::Initialize] No valid game mode definition found — continuing without game mode");
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
    Logger::Debug("[GameServer::Initialize] Starting periodic handler regeneration (3600s interval)");
    StartAutoRegen(3600);

    m_running = true;
    Logger::Info("GameServer initialized successfully");
    Logger::Trace("[GameServer::Initialize] Exit, returning true");
    return true;
}

void GameServer::Run() {
    Logger::Trace("[GameServer::Run] Entry");
    if (!m_running) {
        Logger::Debug("[GameServer::Run] Server not running, skipping tick");
        Logger::Trace("[GameServer::Run] Exit (not running)");
        return;
    }

    // Start frame timing
    auto frameStart = std::chrono::high_resolution_clock::now();

    // Poll network and process received packets (with timing)
    {
        auto netStart = std::chrono::high_resolution_clock::now();
        if (m_networkManager) {
            m_networkManager->PollNetwork();
        }
        auto netEnd = std::chrono::high_resolution_clock::now();
        double netMs = std::chrono::duration<double, std::milli>(netEnd - netStart).count();
        Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateFrameTiming(0, 0, netMs, 0);
    }

    // Process queued packets
    auto packets = FetchPendingPackets();
    Logger::Trace("[GameServer::Run] Processing %zu queued packets", packets.size());
    for (auto& qpkt : packets) {
        auto conn = GetClientConnection(qpkt.clientId);
        if (!conn) {
            Logger::Debug("[GameServer::Run] No connection for clientId=%u, skipping packet", qpkt.clientId);
            continue;
        }

        std::string tag = qpkt.packet.GetTag();
        Logger::Debug("[GameServer::Run] Processing packet tag='%s' from clientId=%u", tag.c_str(), qpkt.clientId);
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
            Logger::Debug("[GameServer::Run] Forwarding unhandled tag '%s' to GameMode", tag.c_str());
            m_gameMode->HandlePlayerAction(qpkt.clientId, tag, qpkt.packet.RawData());
        } else {
            Logger::Debug("[GameServer::Run] Unhandled packet tag '%s' and no GameMode available", tag.c_str());
        }
    }

    float dt = m_tickDeltaSeconds;

    // Tick core subsystems with game logic timing
    {
        auto gameStart = std::chrono::high_resolution_clock::now();

        if (m_gameMode) m_gameMode->Update();
        if (m_playerManager) m_playerManager->Update();

        // Tick RS2V game systems
        if (m_ticketSystem) m_ticketSystem->Update(dt);
        if (m_objectiveSystem) m_objectiveSystem->Update(dt);
        if (m_commanderAbilities) m_commanderAbilities->Update(dt);
        if (m_spawnSystem) m_spawnSystem->Update(dt);
        if (m_damageSystem) m_damageSystem->Update(dt);

        auto gameEnd = std::chrono::high_resolution_clock::now();
        double gameMs = std::chrono::duration<double, std::milli>(gameEnd - gameStart).count();

        // Physics timing (helicopter physics is the main physics update)
        auto physStart = std::chrono::high_resolution_clock::now();
        if (m_helicopterPhysics) m_helicopterPhysics->Update(dt);
        auto physEnd = std::chrono::high_resolution_clock::now();
        double physMs = std::chrono::duration<double, std::milli>(physEnd - physStart).count();

        // Tick active game mode
        if (m_territoryMode) m_territoryMode->Update(dt);
        if (m_supremacyMode) m_supremacyMode->Update(dt);
        if (m_skirmishMode) m_skirmishMode->Update(dt);

        // Update performance metrics with timing data
        auto frameEnd = std::chrono::high_resolution_clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateFrameTiming(
            frameMs, physMs, 0, gameMs);
    }

    if (m_networkManager) m_networkManager->Flush();
    Logger::Trace("[GameServer::Run] Exit");
}

void GameServer::Shutdown() {
    Logger::Trace("[GameServer::Shutdown] Entry");
    if (!m_running) {
        Logger::Debug("[GameServer::Shutdown] Already shut down, skipping");
        Logger::Trace("[GameServer::Shutdown] Exit (already shut down)");
        return;
    }
    Logger::Info("Shutting down GameServer...");

    m_running = false;
    StopAutoRegen();

    // Shutdown RS2V systems
    Logger::Debug("[GameServer::Shutdown] Destroying RS2V game systems");
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
        Logger::Debug("[GameServer::Shutdown] Ending active GameMode");
        m_gameMode->OnEnd();
        m_gameMode.reset();
    } else {
        Logger::Debug("[GameServer::Shutdown] No active GameMode to end");
    }
    m_chatManager.reset();
    if (m_adminManager) {
        Logger::Debug("[GameServer::Shutdown] Shutting down AdminManager");
        m_adminManager->Shutdown();
        m_adminManager.reset();
    }
    m_mapManager.reset();
    m_teamManager.reset();
    m_playerManager.reset();
    if (m_networkManager) {
        Logger::Debug("[GameServer::Shutdown] Shutting down NetworkManager");
        m_networkManager->Shutdown();
        m_networkManager.reset();
    }

    Logger::Info("GameServer shutdown complete");
    Logger::Trace("[GameServer::Shutdown] Exit");
}

void GameServer::BroadcastChatMessage(const std::string& msg) {
    Logger::Trace("[GameServer::BroadcastChatMessage] Entry, msg='%s'", msg.c_str());
    if (m_chatManager) {
        Logger::Debug("[GameServer::BroadcastChatMessage] Broadcasting via ChatManager");
        m_chatManager->BroadcastChat(msg);
    } else {
        Logger::Debug("[GameServer::BroadcastChatMessage] No ChatManager available, message dropped");
    }
    Logger::Trace("[GameServer::BroadcastChatMessage] Exit");
}

uint32_t GameServer::FindClientBySteamID(const std::string& steamId) const {
    Logger::Trace("[GameServer::FindClientBySteamID] Entry, steamId='%s'", steamId.c_str());
    uint32_t result = m_networkManager ? m_networkManager->FindClientBySteamID(steamId) : UINT32_MAX;
    if (result == UINT32_MAX) {
        Logger::Debug("[GameServer::FindClientBySteamID] No client found for steamId='%s'", steamId.c_str());
    } else {
        Logger::Debug("[GameServer::FindClientBySteamID] Found clientId=%u for steamId='%s'", result, steamId.c_str());
    }
    Logger::Trace("[GameServer::FindClientBySteamID] Exit, returning %u", result);
    return result;
}

std::shared_ptr<ClientConnection> GameServer::GetClientConnection(uint32_t clientId) const {
    Logger::Trace("[GameServer::GetClientConnection] Entry, clientId=%u", clientId);
    auto result = m_networkManager ? m_networkManager->GetConnection(clientId) : nullptr;
    if (!result) {
        Logger::Debug("[GameServer::GetClientConnection] No connection found for clientId=%u", clientId);
    }
    Logger::Trace("[GameServer::GetClientConnection] Exit, result=%s", result ? "valid" : "null");
    return result;
}

std::vector<std::shared_ptr<ClientConnection>> GameServer::GetAllConnections() const {
    Logger::Trace("[GameServer::GetAllConnections] Entry");
    auto result = m_networkManager ? m_networkManager->GetAllConnections() : std::vector<std::shared_ptr<ClientConnection>>{};
    Logger::Trace("[GameServer::GetAllConnections] Exit, returning %zu connections", result.size());
    return result;
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
    Logger::Trace("[GameServer::ChangeMap] Entry");
    std::string nextMap = m_mapManager->GetNextMap();
    Logger::Info("[GameServer::ChangeMap] Changing to next map: '%s'", nextMap.c_str());
    if (m_mapManager->LoadMap(nextMap)) {
        Logger::Debug("[GameServer::ChangeMap] Map loaded successfully, transitioning GameMode");
        if (m_gameMode) {
            Logger::Debug("[GameServer::ChangeMap] Ending current GameMode");
            m_gameMode->OnEnd();
        }
        const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
        if (gmDef) {
            Logger::Debug("[GameServer::ChangeMap] Creating new GameMode instance");
            m_gameMode = std::make_unique<GameMode>(this, *gmDef);
            m_gameMode->OnStart();
        } else {
            Logger::Warn("[GameServer::ChangeMap] No valid game mode definition found after map change");
        }
    } else {
        Logger::Error("[GameServer::ChangeMap] Failed to change to map: %s", nextMap.c_str());
    }
    Logger::Trace("[GameServer::ChangeMap] Exit");
}

// Packet queue implementation
void GameServer::EnqueuePacket(const QueuedPacket& pkt) {
    Logger::Trace("[GameServer::EnqueuePacket] Entry, clientId=%u", pkt.clientId);
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    m_packetQueue.push(pkt);
    Logger::Trace("[GameServer::EnqueuePacket] Exit");
}

std::vector<QueuedPacket> GameServer::FetchPendingPackets() {
    Logger::Trace("[GameServer::FetchPendingPackets] Entry");
    std::vector<QueuedPacket> out;
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    while (!m_packetQueue.empty()) {
        out.push_back(std::move(m_packetQueue.front()));
        m_packetQueue.pop();
    }
    Logger::Trace("[GameServer::FetchPendingPackets] Exit, returning %zu packets", out.size());
    return out;
}

void GameServer::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    Logger::Trace("[GameServer::OnPacketReceived] Entry, clientId=%u", clientId);
    EnqueuePacket({clientId, pkt, meta});
    Logger::Trace("[GameServer::OnPacketReceived] Exit");
}

void GameServer::ProcessNetworkMessages() {
    Logger::Trace("[GameServer::ProcessNetworkMessages] Entry");
    if (m_networkManager) {
        m_networkManager->PollNetwork();
    } else {
        Logger::Debug("[GameServer::ProcessNetworkMessages] No NetworkManager available");
    }
    Logger::Trace("[GameServer::ProcessNetworkMessages] Exit");
}

std::string GameServer::GetExeDir() const {
    Logger::Trace("[GameServer::GetExeDir] Entry");
    std::string result = PathUtils::GetExecutableDirectory();
    Logger::Trace("[GameServer::GetExeDir] Exit, returning '%s'", result.c_str());
    return result;
}

// ============================================================================
// RS2V Packet Handlers
// ============================================================================

void GameServer::HandleRoleSelection(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleRoleSelection] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_roleSystem || data.size() < 1) {
        Logger::Debug("[GameServer::HandleRoleSelection] No role system or insufficient data");
        Logger::Trace("[GameServer::HandleRoleSelection] Exit (early return)");
        return;
    }

    CombatRole role = static_cast<CombatRole>(data[0]);
    Logger::Debug("[GameServer::HandleRoleSelection] Player %u requesting role %d", clientId, static_cast<int>(role));
    if (m_roleSystem->AssignRole(clientId, role)) {
        Logger::Info("Player %u selected role: %s", clientId, m_roleSystem->GetRoleName(role).c_str());

        // Apply role loadout to player
        auto* tm = GetTeamManager();
        Faction faction = m_roleSystem->GetTeamFaction(tm->GetPlayerTeam(clientId));
        RoleLoadout loadout = m_roleSystem->GetRoleLoadout(role, faction);
        Logger::Debug("[GameServer::HandleRoleSelection] Applying loadout: primary='%s', secondary='%s'",
                     loadout.primaryWeapon.c_str(), loadout.secondaryWeapon.c_str());

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
            Logger::Debug("[GameServer::HandleRoleSelection] Loadout applied to player %u", clientId);
        } else {
            Logger::Warn("[GameServer::HandleRoleSelection] Player %u not found in PlayerManager", clientId);
        }
    } else {
        Logger::Warn("Player %u role selection failed: role %d", clientId, static_cast<int>(role));
    }
    Logger::Trace("[GameServer::HandleRoleSelection] Exit");
}

void GameServer::HandleSpawnRequest(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleSpawnRequest] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_spawnSystem) {
        Logger::Debug("[GameServer::HandleSpawnRequest] No spawn system available");
        Logger::Trace("[GameServer::HandleSpawnRequest] Exit (no spawn system)");
        return;
    }

    if (data.size() >= 4) {
        uint32_t spawnLocId = 0;
        memcpy(&spawnLocId, data.data(), sizeof(uint32_t));
        Logger::Debug("[GameServer::HandleSpawnRequest] Player %u requesting spawn at location %u", clientId, spawnLocId);
        if (m_spawnSystem->SpawnPlayer(clientId, spawnLocId)) {
            Logger::Debug("Player %u spawned at location %u", clientId, spawnLocId);
        } else {
            Logger::Debug("[GameServer::HandleSpawnRequest] Spawn at location %u failed, trying default spawn", spawnLocId);
            // Try default spawn
            m_spawnSystem->SpawnPlayerAtDefault(clientId);
        }
    } else {
        Logger::Debug("[GameServer::HandleSpawnRequest] Insufficient data for location, using default spawn");
        m_spawnSystem->SpawnPlayerAtDefault(clientId);
    }
    Logger::Trace("[GameServer::HandleSpawnRequest] Exit");
}

void GameServer::HandleCommanderAbility(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleCommanderAbility] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_commanderAbilities || !m_roleSystem) {
        Logger::Debug("[GameServer::HandleCommanderAbility] Missing commander abilities or role system");
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (missing systems)");
        return;
    }

    // Verify player is commander
    if (m_roleSystem->GetPlayerRole(clientId) != CombatRole::Commander) {
        Logger::Warn("Player %u tried commander ability but is not commander", clientId);
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (not commander)");
        return;
    }

    if (data.size() < 13) {
        Logger::Debug("[GameServer::HandleCommanderAbility] Insufficient data (need 13, got %zu)", data.size());
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (insufficient data)");
        return;
    }  // 1 byte type + 12 bytes position

    AbilityType type = static_cast<AbilityType>(data[0]);
    Vector3 target;
    memcpy(&target.x, data.data() + 1, sizeof(float));
    memcpy(&target.y, data.data() + 5, sizeof(float));
    memcpy(&target.z, data.data() + 9, sizeof(float));
    Logger::Debug("[GameServer::HandleCommanderAbility] Ability type=%d, target=(%.1f, %.1f, %.1f)",
                 static_cast<int>(type), target.x, target.y, target.z);

    Vector3 direction;
    if (data.size() >= 25) {
        memcpy(&direction.x, data.data() + 13, sizeof(float));
        memcpy(&direction.y, data.data() + 17, sizeof(float));
        memcpy(&direction.z, data.data() + 21, sizeof(float));
        Logger::Debug("[GameServer::HandleCommanderAbility] Direction=(%.1f, %.1f, %.1f)",
                     direction.x, direction.y, direction.z);
    } else {
        Logger::Debug("[GameServer::HandleCommanderAbility] No direction data provided");
    }

    if (m_commanderAbilities->RequestAbility(clientId, type, target, direction)) {
        Logger::Info("Commander %u called %s at (%.1f, %.1f, %.1f)",
                     clientId, m_commanderAbilities->GetAbilityName(type).c_str(),
                     target.x, target.y, target.z);
    } else {
        Logger::Debug("[GameServer::HandleCommanderAbility] Ability request denied for commander %u", clientId);
    }
    Logger::Trace("[GameServer::HandleCommanderAbility] Exit");
}

void GameServer::HandleSquadAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleSquadAction] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_roleSystem || data.size() < 1) {
        Logger::Debug("[GameServer::HandleSquadAction] No role system or insufficient data");
        Logger::Trace("[GameServer::HandleSquadAction] Exit (early return)");
        return;
    }

    uint8_t action = data[0];
    Logger::Debug("[GameServer::HandleSquadAction] Player %u squad action=%u", clientId, action);
    switch (action) {
        case 0: {  // Create squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Create squad");
            uint32_t teamId = GetTeamManager()->GetPlayerTeam(clientId);
            uint32_t squadId = m_roleSystem->CreateSquad(teamId);
            if (squadId > 0) {
                m_roleSystem->JoinSquad(clientId, squadId);
                Logger::Info("Player %u created and joined squad %u", clientId, squadId);
            } else {
                Logger::Warn("[GameServer::HandleSquadAction] Squad creation failed for player %u on team %u", clientId, teamId);
            }
            break;
        }
        case 1: {  // Join squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Join squad");
            if (data.size() >= 5) {
                uint32_t squadId = 0;
                memcpy(&squadId, data.data() + 1, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleSquadAction] Player %u joining squad %u", clientId, squadId);
                m_roleSystem->JoinSquad(clientId, squadId);
            } else {
                Logger::Debug("[GameServer::HandleSquadAction] Insufficient data for join squad action");
            }
            break;
        }
        case 2:  // Leave squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Leave squad, player %u", clientId);
            m_roleSystem->LeaveSquad(clientId);
            break;
        case 3:  // Volunteer as commander
            Logger::Debug("[GameServer::HandleSquadAction] Action: Volunteer as commander, player %u", clientId);
            m_roleSystem->VolunteerAsCommander(clientId);
            break;
        case 4:  // Resign as commander
            Logger::Debug("[GameServer::HandleSquadAction] Action: Resign as commander, player %u", clientId);
            m_roleSystem->ResignAsCommander(clientId);
            break;
        default:
            Logger::Debug("[GameServer::HandleSquadAction] Unknown squad action %u from player %u", action, clientId);
            break;
    }
    Logger::Trace("[GameServer::HandleSquadAction] Exit");
}

void GameServer::HandleVehicleAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleVehicleAction] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_helicopterPhysics || data.size() < 1) {
        Logger::Debug("[GameServer::HandleVehicleAction] No helicopter physics or insufficient data");
        Logger::Trace("[GameServer::HandleVehicleAction] Exit (early return)");
        return;
    }

    uint8_t action = data[0];
    Logger::Debug("[GameServer::HandleVehicleAction] Player %u vehicle action=%u", clientId, action);
    switch (action) {
        case 0: {  // Enter helicopter
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Enter helicopter");
            if (data.size() >= 9) {
                uint32_t heliId = 0, seat = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                memcpy(&seat, data.data() + 5, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleVehicleAction] Player %u entering heli %u seat %u", clientId, heliId, seat);
                m_helicopterPhysics->EnterHelicopter(clientId, heliId, seat);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for enter helicopter");
            }
            break;
        }
        case 1:  // Exit helicopter
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Exit helicopter, player %u", clientId);
            m_helicopterPhysics->ExitHelicopter(clientId);
            break;
        case 2: {  // Control input
            Logger::Trace("[GameServer::HandleVehicleAction] Action: Control input");
            if (data.size() >= 18) {
                HeliControlInput input;
                memcpy(&input.collective, data.data() + 1, sizeof(float));
                memcpy(&input.cyclic_pitch, data.data() + 5, sizeof(float));
                memcpy(&input.cyclic_roll, data.data() + 9, sizeof(float));
                memcpy(&input.pedal, data.data() + 13, sizeof(float));
                input.fireWeapon = (data.size() > 17 && data[17] != 0);
                // Find helicopter this player is piloting
                bool found = false;
                for (auto* h : m_helicopterPhysics->GetAllHelicopters()) {
                    if (h->pilotId == clientId) {
                        m_helicopterPhysics->SetControlInput(h->vehicleId, input);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    Logger::Debug("[GameServer::HandleVehicleAction] Player %u not piloting any helicopter", clientId);
                }
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for control input");
            }
            break;
        }
        case 3: {  // Start engine
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Start engine");
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleVehicleAction] Starting engine on heli %u", heliId);
                m_helicopterPhysics->StartEngine(heliId);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for start engine");
            }
            break;
        }
        case 4: {  // Stop engine
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Stop engine");
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleVehicleAction] Stopping engine on heli %u", heliId);
                m_helicopterPhysics->StopEngine(heliId);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for stop engine");
            }
            break;
        }
        default:
            Logger::Debug("[GameServer::HandleVehicleAction] Unknown vehicle action %u from player %u", action, clientId);
            break;
    }
    Logger::Trace("[GameServer::HandleVehicleAction] Exit");
}

void GameServer::HandleWeaponFire(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleWeaponFire] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_damageSystem || data.size() < 29) {
        Logger::Debug("[GameServer::HandleWeaponFire] No damage system or insufficient data (need 29, got %zu)", data.size());
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (early return)");
        return;
    }

    // Parse weapon fire data: weaponId(variable) + origin(12) + direction(12) + hitZone(1) + victimId(4)
    // Simplified: first byte = weapon id string length
    uint8_t weaponIdLen = data[0];
    if (data.size() < static_cast<size_t>(1 + weaponIdLen + 29)) {
        Logger::Debug("[GameServer::HandleWeaponFire] Data too short for weapon id len %u", weaponIdLen);
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (data too short)");
        return;
    }

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

    Logger::Debug("[GameServer::HandleWeaponFire] Player %u fired '%s' at victim %u, hitZone=%d",
                 clientId, weaponId.c_str(), victimId, static_cast<int>(hitZone));

    if (victimId == 0) {
        Logger::Debug("[GameServer::HandleWeaponFire] Miss (victimId=0), no damage applied");
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (miss)");
        return;  // Miss — no damage
    }

    // Calculate damage using weapon database
    float distance = origin.Distance(m_playerManager->GetPlayer(victimId) ?
                     m_playerManager->GetPlayer(victimId)->GetPosition() : origin);

    auto* weaponDef = m_weaponDatabase ? m_weaponDatabase->GetWeapon(weaponId) : nullptr;
    if (!weaponDef) {
        Logger::Warn("[GameServer::HandleWeaponFire] Weapon '%s' not found in database, using default damage", weaponId.c_str());
    }
    float baseDmg = weaponDef ? m_weaponDatabase->CalculateDamage(
        weaponId, distance, hitZone == HitZone::Head,
        hitZone == HitZone::LeftArm || hitZone == HitZone::RightArm ||
        hitZone == HitZone::LeftLeg || hitZone == HitZone::RightLeg) : 50.0f;

    Logger::Debug("[GameServer::HandleWeaponFire] Calculated damage=%.1f, distance=%.1f", baseDmg, distance);

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
    Logger::Trace("[GameServer::HandleWeaponFire] Exit");
}
