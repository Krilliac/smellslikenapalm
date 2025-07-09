// src/Game/GameServer.cpp – Complete implementation for RS2V Server core

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
#include <chrono>
#include <thread>

GameServer::GameServer()
{
    Logger::Info("GameServer constructed");
}

GameServer::~GameServer()
{
    Shutdown();
}

bool GameServer::Initialize()
{
    Logger::Info("GameServer initialization starting...");

    // Load configurations
    m_configManager = std::make_shared<ConfigManager>();
    if (!m_configManager->Initialize()) return false;
    if (!m_configManager->LoadConfiguration("config/server.ini")) return false;

    m_serverConfig = std::make_shared<ServerConfig>();
    if (!m_serverConfig->Initialize("config/server.ini")) return false;

    m_networkConfig = std::make_shared<NetworkConfig>();
    if (!m_networkConfig->Initialize(m_configManager)) return false;

    m_securityConfig = std::make_shared<SecurityConfig>();
    if (!m_securityConfig->Initialize(m_configManager)) return false;

    m_gameConfig = std::make_shared<GameConfig>();
    if (!m_gameConfig->Initialize(m_configManager)) return false;

    m_mapConfig = std::make_shared<MapConfig>();
    if (!m_mapConfig->Initialize("config/maps.ini")) return false;

    // Initialize managers
    m_networkManager = std::make_unique<NetworkManager>(m_networkConfig);
    if (!m_networkManager->Initialize()) return false;

    m_playerManager = std::make_unique<PlayerManager>(this);
    m_teamManager   = std::make_unique<TeamManager>(this);
    m_mapManager    = std::make_unique<MapManager>(this, m_mapConfig);

    m_adminManager  = std::make_unique<AdminManager>(this, m_serverConfig);
    m_adminManager->Initialize();

    m_chatManager   = std::make_unique<ChatManager>(this);
    m_chatManager->Initialize();

    // Initialize EAC / Telemetry proxy if enabled
    if (m_configManager->GetBool("EAC.EnableEACProxy", false)) {
        m_eacProxy = std::make_unique<EACProxy>(m_configManager);
        m_eacProxy->Initialize();
    }

    // Start first map and game mode
    std::string mapName = m_gameConfig->GetGameSettings().mapName;
    if (!m_mapManager->LoadMap(mapName)) {
        Logger::Error("Failed to load map: %s", mapName.c_str());
        return false;
    }

    const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
    if (!gmDef) {
        Logger::Error("Invalid game mode: %s", m_gameConfig->GetGameSettings().gameMode.c_str());
        return false;
    }

    m_gameMode = std::make_unique<GameMode>(this, *gmDef);
    m_gameMode->OnStart();

    Logger::Info("GameServer initialized successfully");
    return true;
}

void GameServer::Run()
{
    Logger::Info("GameServer main loop starting");
    m_running = true;

    auto lastTime = std::chrono::steady_clock::now();
    const double tickInterval = 1.0 / m_serverConfig->GetTickRate();

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> delta = now - lastTime;
        if (delta.count() >= tickInterval) {
            lastTime = now;

            // Network receive
            auto packets = m_networkManager->ReceivePackets();
            for (auto& pkt : packets) {
                uint32_t clientId = m_networkManager->GetClientId(pkt.source);
                if (m_adminManager->IsBanned(pkt.source.steamId)) {
                    m_networkManager->Disconnect(clientId, "Banned");
                    continue;
                }
                if (pkt.isChat) {
                    m_chatManager->ProcessChatCommand(clientId, pkt.payload);
                } else {
                    m_gameMode->HandlePlayerAction(clientId, pkt.type, pkt.data);
                }
            }

            // Game update
            m_gameMode->Update();

            // Periodic tasks
            m_playerManager->Update();
            m_teamManager->Update();
            m_networkManager->Flush(); 
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Logger::Info("GameServer main loop exited");
}

void GameServer::Shutdown()
{
    if (!m_running) return;
    Logger::Info("Shutting down GameServer...");

    m_running = false;
    if (m_gameMode) {
        m_gameMode->OnEnd();
        m_gameMode.reset();
    }
    if (m_eacProxy) {
        m_eacProxy->Shutdown();
        m_eacProxy.reset();
    }
    m_chatManager.reset();
    m_adminManager->Shutdown();
    m_adminManager.reset();
    m_mapManager.reset();
    m_teamManager.reset();
    m_playerManager.reset();
    m_networkManager->Shutdown();
    m_networkManager.reset();

    Logger::Info("GameServer shutdown complete");
}

void GameServer::BroadcastChatMessage(const std::string& msg)
{
    m_chatManager->BroadcastChat(msg);
}

uint32_t GameServer::FindClientBySteamID(const std::string& steamId) const
{
    return m_networkManager->FindClientBySteamID(steamId);
}

std::shared_ptr<ClientConnection> GameServer::GetClientConnection(uint32_t clientId) const
{
    return m_networkManager->GetConnection(clientId);
}

std::vector<std::shared_ptr<ClientConnection>> GameServer::GetAllConnections() const
{
    return m_networkManager->GetAllConnections();
}

std::shared_ptr<PlayerManager> GameServer::GetPlayerManager() const { return m_playerManager; }
std::shared_ptr<TeamManager>   GameServer::GetTeamManager()   const { return m_teamManager;   }
std::shared_ptr<MapManager>    GameServer::GetMapManager()    const { return m_mapManager;    }
std::shared_ptr<GameConfig>    GameServer::GetGameConfig()    const { return m_gameConfig;    }
std::shared_ptr<ServerConfig>  GameServer::GetServerConfig()  const { return m_serverConfig;  }
std::shared_ptr<ConfigManager> GameServer::GetConfigManager() const { return m_configManager; }

void GameServer::ChangeMap()
{
    // Rotate to next map or reload current
    std::string nextMap = m_mapManager->GetNextMap();
    if (m_mapManager->LoadMap(nextMap)) {
        m_gameMode->OnEnd();
        const auto& gmDef = m_gameConfig->GetGameModeDefinition(m_gameConfig->GetGameSettings().gameMode);
        m_gameMode = std::make_unique<GameMode>(this, *gmDef);
        m_gameMode->OnStart();
    } else {
        Logger::Error("Failed to change to map: %s", nextMap.c_str());
    }
}