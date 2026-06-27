// src/Game/GameServer.h

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
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
class ClientConnection;
class RoleSystem;
class TicketSystem;
class ObjectiveSystem;
class CommanderAbilities;
class SpawnSystem;
class WeaponDatabase;
class DamageSystem;
class ProjectileManager;
class HelicopterPhysics;
class TerritoryMode;
class SupremacyMode;
class SkirmishMode;
class ProtocolHandler;
class ReplicationManager;
class ConnectionLoginBridge;
class SecurityManager;   // forward-declared only; Security headers are NOT included here

struct PacketAnalysisResult;

namespace GeneratedHandlers {
    using HandlerFunction = void(*)(const PacketAnalysisResult&);
    class HandlerLibraryManager;
}

// Internal packet queue entry
struct QueuedPacket {
    uint32_t       clientId;
    Packet         packet;
    PacketMetadata metadata;
};

class GameServer {
public:
    GameServer();
    virtual ~GameServer();

    virtual bool Initialize();
    void Run();       // Run one tick of the game loop (called from main GameClock callback)
    virtual void Shutdown();

    void BroadcastChatMessage(const std::string& msg);

    uint32_t FindClientBySteamID(const std::string& steamId) const;
    std::shared_ptr<ClientConnection> GetClientConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;

    // Subsystem accessors
    PlayerManager*                  GetPlayerManager()      const;
    TeamManager*                    GetTeamManager()        const;
    MapManager*                     GetMapManager()         const;
    NetworkManager*                 GetNetworkManager()     const;
    AdminManager*                   GetAdminManager()       const;
    // The authoritative SecurityManager (lives in the login bridge); may be null if security
    // is unconfigured. Returns a pointer only - GameServer.h never includes Security headers.
    SecurityManager*                GetSecurityManager()    const;
    RoleSystem*                     GetRoleSystem()         const;
    TicketSystem*                   GetTicketSystem()       const;
    ObjectiveSystem*                GetObjectiveSystem()    const;
    CommanderAbilities*             GetCommanderAbilities() const;
    SpawnSystem*                    GetSpawnSystem()        const;
    WeaponDatabase*                 GetWeaponDatabase()     const;
    DamageSystem*                   GetDamageSystem()       const;
    ProjectileManager*              GetProjectileManager()  const;
    HelicopterPhysics*              GetHelicopterPhysics()  const;
    TerritoryMode*                  GetTerritoryMode()      const;
    SupremacyMode*                  GetSupremacyMode()      const;
    SkirmishMode*                   GetSkirmishMode()       const;
    std::shared_ptr<GameConfig>     GetGameConfig()         const;
    std::shared_ptr<ServerConfig>   GetServerConfig()       const;
    std::shared_ptr<ConfigManager>  GetConfigManager()      const;

    void ChangeMap();

    // Packet queue — used by NetworkManager/ConnectionManager to push received packets
    void EnqueuePacket(const QueuedPacket& pkt);
    std::vector<QueuedPacket> FetchPendingPackets();

    // Callback from ConnectionManager when a raw packet arrives
    void OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);

    // --- Packet handler regeneration/reload ---
    void Cmd_RegenHandlers(const std::vector<std::string>& args = {});
    void StartAutoRegen(int intervalSeconds = 600);
    void StopAutoRegen();
    void DynamicReloadGeneratedHandlers();

protected:
    virtual void ProcessNetworkMessages();

    std::shared_ptr<ServerConfig>       m_serverConfig;
    std::shared_ptr<GameConfig>         m_gameConfig;

private:
    std::string GetExeDir() const;

    std::unique_ptr<NetworkManager>     m_networkManager;
    std::shared_ptr<ConfigManager>      m_configManager;
    std::shared_ptr<NetworkConfig>      m_networkConfig;
    std::shared_ptr<SecurityConfig>     m_securityConfig;
    std::shared_ptr<MapConfig>          m_mapConfig;

    std::unique_ptr<PlayerManager>      m_playerManager;
    std::unique_ptr<TeamManager>        m_teamManager;
    std::unique_ptr<MapManager>         m_mapManager;
    std::unique_ptr<AdminManager>       m_adminManager;
    std::unique_ptr<ChatManager>        m_chatManager;
    std::unique_ptr<GameMode>           m_gameMode;

    // RS2V game systems
    std::unique_ptr<RoleSystem>         m_roleSystem;
    std::unique_ptr<TicketSystem>       m_ticketSystem;
    std::unique_ptr<ObjectiveSystem>    m_objectiveSystem;
    std::unique_ptr<CommanderAbilities> m_commanderAbilities;
    std::unique_ptr<SpawnSystem>        m_spawnSystem;
    std::unique_ptr<WeaponDatabase>     m_weaponDatabase;
    std::unique_ptr<DamageSystem>       m_damageSystem;
    std::unique_ptr<ProjectileManager>  m_projectileManager;
    std::unique_ptr<HelicopterPhysics>  m_helicopterPhysics;
    std::unique_ptr<TerritoryMode>      m_territoryMode;
    std::unique_ptr<SupremacyMode>      m_supremacyMode;
    std::unique_ptr<SkirmishMode>       m_skirmishMode;

    // Replication + the connection->player login bridge (the bridge owns the
    // SecurityManager internally; see GameServer::Initialize for why).
    std::unique_ptr<ProtocolHandler>      m_protocolHandler;
    std::unique_ptr<ReplicationManager>   m_replicationManager;
    std::unique_ptr<ConnectionLoginBridge> m_loginBridge;

    // Game tick timing
    float m_lastTickTime = 0.0f;
    float m_tickDeltaSeconds = 1.0f / 60.0f;  // 60Hz default

    // Packet handlers for RS2V systems
    void HandleRoleSelection(uint32_t clientId, const std::vector<uint8_t>& data);
    void HandleSpawnRequest(uint32_t clientId, const std::vector<uint8_t>& data);
    void HandleCommanderAbility(uint32_t clientId, const std::vector<uint8_t>& data);
    void HandleSquadAction(uint32_t clientId, const std::vector<uint8_t>& data);
    void HandleVehicleAction(uint32_t clientId, const std::vector<uint8_t>& data);
    void HandleWeaponFire(uint32_t clientId, const std::vector<uint8_t>& data);

    std::atomic<bool>                   m_running{false};

    // Packet receive queue (thread-safe)
    std::mutex                          m_packetQueueMutex;
    std::queue<QueuedPacket>            m_packetQueue;

    // Handler regen/reload
    std::atomic<bool>                   m_regenRunning{false};
    std::thread                         m_regenThread;
    int                                 m_regenIntervalSeconds = 600;
    std::string                         m_handlerLibraryPath;
};
