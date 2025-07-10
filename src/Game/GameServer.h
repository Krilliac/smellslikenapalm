// src/Game/GameServer.h

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include "Network/Packet.h"

class NetworkManager;
class AdminManager;
class ChatManager;
class GameMode;
class ConfigManager;
class ServerConfig;
class NetworkConfig;
class SecurityConfig;
class GameConfig;
class MapConfig;
class PlayerManager;
class TeamManager;
class MapManager;
class EACProxy;
struct ClientConnection;

namespace GeneratedHandlers {
    using HandlerFunction = void(*)(const PacketAnalysisResult&);
    class HandlerLibraryManager;
}

class GameServer {
public:
    GameServer();
    ~GameServer();

    bool Initialize();
    void Run();
    void Shutdown();

    void BroadcastChatMessage(const std::string& msg);

    uint32_t FindClientBySteamID(const std::string& steamId) const;
    std::shared_ptr<ClientConnection> GetClientConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;

    std::shared_ptr<PlayerManager> GetPlayerManager() const;
    std::shared_ptr<TeamManager>   GetTeamManager()   const;
    std::shared_ptr<MapManager>    GetMapManager()    const;
    std::shared_ptr<GameConfig>    GetGameConfig()    const;
    std::shared_ptr<ServerConfig>  GetServerConfig()  const;
    std::shared_ptr<ConfigManager> GetConfigManager() const;

    void ChangeMap();

    // --- Packet handler regeneration/reload --- TODO: Enable/Disable by config
    void Cmd_RegenHandlers(const std::vector<std::string>& args = {});
    void StartAutoRegen(int intervalSeconds = 600); // Production should be once every 3600 seconds (1 hour), but for development, we'll keep it at every 600 seconds (10 minutes.) TODO: Make this into a config.
    void StopAutoRegen();
    void DynamicReloadGeneratedHandlers();

private:
    std::string GetExeDir() const;

    std::unique_ptr<NetworkManager>     m_networkManager;
    std::unique_ptr<EACProxy>           m_eacProxy;
    std::shared_ptr<ConfigManager>      m_configManager;
    std::shared_ptr<ServerConfig>       m_serverConfig;
    std::shared_ptr<NetworkConfig>      m_networkConfig;
    std::shared_ptr<SecurityConfig>     m_securityConfig;
    std::shared_ptr<GameConfig>         m_gameConfig;
    std::shared_ptr<MapConfig>          m_mapConfig;

    std::unique_ptr<PlayerManager>      m_playerManager;
    std::unique_ptr<TeamManager>        m_teamManager;
    std::unique_ptr<MapManager>         m_mapManager;
    std::unique_ptr<AdminManager>       m_adminManager;
    std::unique_ptr<ChatManager>        m_chatManager;
    std::unique_ptr<GameMode>           m_gameMode;

    std::atomic<bool>                   m_running{false};

    // Handler regen/reload
    std::atomic<bool>                   m_regenRunning{false};
    std::thread                         m_regenThread;
    int                                 m_regenIntervalSeconds = 600;
    std::string                         m_handlerLibraryPath;
};