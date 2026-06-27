// src/Game/GameServer.h

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include "Network/Packet.h"
#include "Game/BanRecord.h"

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
class MapVoteManager;
class WorkshopManager;
class ModManager;
class MutatorManager;
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
class GameState;
class RoundManager;
class ProtocolHandler;
class ReplicationManager;
class ConnectionLoginBridge;
class CommandManager;
class ConsoleInput;
class RemoteAdminServer;

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

    // --- Ban administration (single authoritative store, owned by the security
    // layer behind the login bridge). AdminManager and the command system go
    // through here so there is exactly one ban store. durationMinutes <= 0 is a
    // permanent ban. No-ops returning false/empty before the bridge exists. ---
    bool BanSteamId(const std::string& steamId, int durationMinutes, const std::string& reason);
    bool UnbanSteamId(const std::string& steamId);
    bool IsSteamIdBanned(const std::string& steamId) const;
    std::vector<BanRecord> GetActiveBans() const;

    uint32_t FindClientBySteamID(const std::string& steamId) const;
    std::shared_ptr<ClientConnection> GetClientConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;

    // Subsystem accessors
    PlayerManager*                  GetPlayerManager()      const;
    TeamManager*                    GetTeamManager()        const;
    MapManager*                     GetMapManager()         const;
    MapVoteManager*                 GetMapVoteManager()     const;
    WorkshopManager*                GetWorkshopManager()    const;
    ModManager*                     GetModManager()         const;
    MutatorManager*                 GetMutatorManager()     const;
    NetworkManager*                 GetNetworkManager()     const;
    AdminManager*                   GetAdminManager()       const;
    ChatManager*                    GetChatManager()        const;
    CommandManager*                 GetCommandManager()     const;
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
    GameState*                      GetGameState()          const;
    RoundManager*                   GetRoundManager()       const;
    std::shared_ptr<GameConfig>     GetGameConfig()         const;
    std::shared_ptr<ServerConfig>   GetServerConfig()       const;
    std::shared_ptr<ConfigManager>  GetConfigManager()      const;

    void ChangeMap();

    // --- Runtime controls driven by the command system ---
    // Graceful shutdown requested by the `shutdown` command (or a transport).
    // The main loop polls IsShutdownRequested() and stops the game clock.
    void RequestShutdown();
    bool IsShutdownRequested() const { return m_shutdownRequested.load(); }

    // Tick rate is owned by the GameClock in main(); a hook lets the `tickrate`
    // command reach it without GameServer depending on GameClock. SetTickRate
    // applies the hook (if installed) and records the value for `status`.
    void SetTickRateHook(std::function<void(int)> hook);
    bool SetTickRate(int rate);
    int  GetTickRate() const { return m_currentTickRate; }

    // Simulation speed multiplier applied to the per-tick delta (1.0 = normal).
    // Used by the `timescale` dev command for slow-mo / fast-forward testing.
    void  SetTimeScale(float scale);
    float GetTimeScale() const { return m_timeScale; }

    // Map voting. StartMapVote begins an end-of-round vote (using the current
    // map as the excluded option); CastMapVote records a client's pick; the
    // winning map is consumed by the next ChangeMap().
    bool StartMapVote();
    bool CastMapVote(uint32_t clientId, int optionIndex);

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

    // Register the current map's objective definitions with the ObjectiveSystem,
    // applying linear territory ordering when the active mode is Territory.
    void PopulateObjectivesFromMap();

    std::unique_ptr<NetworkManager>     m_networkManager;
    std::shared_ptr<ConfigManager>      m_configManager;
    std::shared_ptr<NetworkConfig>      m_networkConfig;
    std::shared_ptr<SecurityConfig>     m_securityConfig;
    std::shared_ptr<MapConfig>          m_mapConfig;

    std::unique_ptr<PlayerManager>      m_playerManager;
    std::unique_ptr<TeamManager>        m_teamManager;
    std::unique_ptr<MapManager>         m_mapManager;
    std::unique_ptr<MapVoteManager>     m_mapVoteManager;
    std::unique_ptr<WorkshopManager>    m_workshopManager;
    std::unique_ptr<ModManager>         m_modManager;
    std::unique_ptr<MutatorManager>     m_mutatorManager;
    // The map chosen by the most recent concluded vote; consumed by ChangeMap().
    std::string                         m_pendingVoteWinner;
    std::unique_ptr<AdminManager>       m_adminManager;
    std::unique_ptr<ChatManager>        m_chatManager;
    std::unique_ptr<CommandManager>     m_commandManager;
    std::unique_ptr<ConsoleInput>       m_consoleInput;
    std::unique_ptr<RemoteAdminServer>  m_remoteAdminServer;
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

    // Central round/game-state layer. Opt-in (Game.use_round_manager): when
    // enabled, RoundManager drives the generic Preparation/Active/PostRound
    // cycle over GameState. Off by default so the per-mode classes above remain
    // the sole round drivers.
    std::unique_ptr<GameState>          m_gameState;
    std::unique_ptr<RoundManager>       m_roundManager;

    // Replication + the connection->player login bridge (the bridge owns the
    // SecurityManager internally; see GameServer::Initialize for why).
    std::unique_ptr<ProtocolHandler>      m_protocolHandler;
    std::unique_ptr<ReplicationManager>   m_replicationManager;
    std::unique_ptr<ConnectionLoginBridge> m_loginBridge;

    // Game tick timing
    float m_lastTickTime = 0.0f;
    float m_tickDeltaSeconds = 1.0f / 60.0f;  // 60Hz default
    float m_timeScale = 1.0f;                  // command-controlled sim speed

    // Runtime control state driven by the command system.
    std::atomic<bool>          m_shutdownRequested{false};
    std::function<void(int)>   m_tickRateHook;        // installed by main()
    int                        m_currentTickRate = 60;

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
