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
#include "Utils/PathUtils.h"
#include "Utils/HandlerLibraryManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include <chrono>
#include <thread>
#include <cstdlib>
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
        } else if (m_gameMode) {
            m_gameMode->HandlePlayerAction(qpkt.clientId, tag, qpkt.packet.RawData());
        }
    }

    // Tick subsystems
    if (m_gameMode) m_gameMode->Update();
    if (m_playerManager) m_playerManager->Update();
    // TeamManager has no per-tick Update
    if (m_networkManager) m_networkManager->Flush();
}

void GameServer::Shutdown() {
    if (!m_running) return;
    Logger::Info("Shutting down GameServer...");

    m_running = false;
    StopAutoRegen();

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
PlayerManager*  GameServer::GetPlayerManager()  const { return m_playerManager.get(); }
TeamManager*    GameServer::GetTeamManager()    const { return m_teamManager.get();   }
MapManager*     GameServer::GetMapManager()     const { return m_mapManager.get();    }
NetworkManager* GameServer::GetNetworkManager() const { return m_networkManager.get();}
AdminManager*   GameServer::GetAdminManager()   const { return m_adminManager.get();  }

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
