// src/Game/GameServer.h

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <functional>
#include "Math/Vector3.h"
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
namespace CombatAuthority {
class Authority;
struct CombatEvent;
}
namespace MantleAuthority {
class Authority;
}
namespace MantleRepl {
struct Attempt;
struct DynamicInfo;
}
namespace WeaponCombatRepl {
struct ServerHandleClientHitsOne;
}
class ProjectileManager;
class HelicopterPhysics;
class TerritoryMode;
class SupremacyMode;
class SkirmishMode;
class GameState;
class RoundManager;
class BotManager;
struct BotDeathEvent;
struct BotHumanCombatEvent;
class ReplicationManager;
class ConnectionLoginBridge;
class SecurityManager;   // forward-declared only; Security headers are NOT included here
class CommandManager;
class ConsoleInput;
class RemoteAdminServer;
class GameServerBotCombatTestHarness;
class GameServerNativeRotationTestHarness;

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
    // Initialize from the selected primary config and apply optional game/EAC
    // port overrides before the corresponding sockets bind. Zero means no
    // command-line override and preserves the selected config value.
    bool Initialize(const std::string& configFile, uint16_t portOverride,
                    const std::string& mapOverride = {},
                    uint16_t eacPortOverride = 0);
    // Run one game-loop iteration. Production passes the monotonic elapsed time
    // measured by GameClock. Omitting it retains a deterministic nominal tick
    // for direct tests/tools that drive the server manually.
    void Run(float elapsedSeconds = -1.0f);
    virtual void Shutdown();

    virtual void BroadcastChatMessage(const std::string& msg);

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
    // The authoritative SecurityManager (lives in the login bridge); may be null if security
    // is unconfigured. Returns a pointer only - GameServer.h never includes Security headers.
    SecurityManager*                GetSecurityManager()    const;
    ChatManager*                    GetChatManager()        const;
    CommandManager*                 GetCommandManager()     const;
    RoleSystem*                     GetRoleSystem()         const;
    TicketSystem*                   GetTicketSystem()       const;
    ObjectiveSystem*                GetObjectiveSystem()    const;
    CommanderAbilities*             GetCommanderAbilities() const;
    SpawnSystem*                    GetSpawnSystem()        const;
    WeaponDatabase*                 GetWeaponDatabase()     const;
    DamageSystem*                   GetDamageSystem()       const;
    CombatAuthority::Authority*     GetCombatAuthority()    const;
    ProjectileManager*              GetProjectileManager()  const;
    HelicopterPhysics*              GetHelicopterPhysics()  const;
    BotManager*                     GetBotManager()         const;
    TerritoryMode*                  GetTerritoryMode()      const;
    SupremacyMode*                  GetSupremacyMode()      const;
    SkirmishMode*                   GetSkirmishMode()       const;
    GameState*                      GetGameState()          const;
    RoundManager*                   GetRoundManager()       const;
    std::shared_ptr<GameConfig>     GetGameConfig()         const;
    std::shared_ptr<ServerConfig>   GetServerConfig()       const;
    std::shared_ptr<ConfigManager>  GetConfigManager()      const;

    void ChangeMap();
    // A gameplay driver can finish from inside its Update. Defer the
    // destructive ChangeMap call until that update returns so no mode is reset
    // while one of its member functions is still on the stack.
    void RequestMapChange();
    // Queue one explicit admin-selected target through the same deferred,
    // validated ClientTravel seam. Returns false with a user-facing reason
    // when the request cannot be admitted synchronously.
    bool RequestMapChange(const std::string& mapName, std::string& error);

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

    // Network-session teardown hook. ConnectionManager invokes this before it
    // removes a timed-out or superseded ClientConnection.
    void OnClientDisconnected(uint32_t clientId);

    // ClientTravel keeps the old protocol session alive only to drain reliable
    // ACKs. Retire its Player/Team/role/combat presence immediately so it cannot
    // affect capacity, bot fill, objectives, or deployment in the new world.
    // The later protocol teardown is intentionally idempotent.
    void OnClientMapTravelQueued(uint32_t clientId);

    // Retail combat authority lifecycle. PlayerManager owns the human-player
    // lifecycle; the exact ROWeapon h56 decoder calls the two request methods.
    // These paths never feed an already-authoritative result back through the
    // legacy DamageSystem, which would apply damage a second time.
    void RegisterCombatParticipant(uint32_t clientId);
    void RemoveCombatParticipant(uint32_t clientId);
    void RespawnCombatParticipant(uint32_t clientId);
    bool HandleRetailCombatHit(
        uint32_t clientId,
        const WeaponCombatRepl::ServerHandleClientHitsOne& rpc);
    bool RequestCombatReload(uint32_t clientId);
    // Select a capture-grounded actor from the fixed owning inventory graph.
    // The class reference is mandatory because ch210/ch212 are stable emulator
    // channels whose concrete actor class differs by faction. The authority
    // validates class/team pairs and fails closed on cross-faction metadata.
    bool SelectCombatWeaponChannel(uint32_t clientId, uint32_t weaponChannel,
                                   uint32_t weaponClassRef);
    // Faction-grenade h29/h30 bridge. Cooking is keyed to CombatAuthority's monotonic
    // server clock; the client supplies only the capture-verified fire mode and
    // a view direction. All projectile characteristics remain server-owned.
    bool BeginRetailGrenadeCook(uint32_t clientId, uint8_t fireMode);
    bool ReleaseRetailGrenade(uint32_t clientId, uint8_t fireMode,
                               const Vector3& aimDirection);
    void CancelRetailGrenadeCook(uint32_t clientId);

    // ROPlayerController h280 admission. ConnectionManager proves the request
    // arrived on the owning PC channel; this method revalidates player, phase,
    // world bounds, geometry, and cooldown before changing authoritative
    // position. Only an accepted result may be encoded as h344.
    bool RequestRetailMantle(uint32_t clientId,
                             const MantleRepl::Attempt& attempt,
                             uint8_t& outSpecialMove,
                             MantleRepl::DynamicInfo& outDynamicInfo);

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
    // Narrow deterministic integration harness; it installs the same concrete
    // combat subsystems used in production without opening server sockets.
    friend class GameServerBotCombatTestHarness;
    // Narrow lifecycle seam for deterministic deferred/native-rotation tests.
    friend class GameServerNativeRotationTestHarness;
    // Narrow role-selection seam for ConnectionManager integration tests. It
    // installs only the concrete player/team/role/spawn authorities and never
    // opens a production listener.
    friend class ConnectionTravelLifecycleTestHarness;

    enum class ActiveModeDriver : uint8_t {
        Territory,
        Supremacy,
        Skirmish,
        Generic
    };

    std::string GetExeDir() const;

    // Resolve one authoritative round/gameplay driver.  A map's default mode is
    // used when Game.game_mode is not explicitly configured; this keeps retail
    // Territory maps from also running the generic Conquest clock plus the
    // Supremacy and Skirmish clocks.
    std::string GetEffectiveGameModeName() const;
    ActiveModeDriver ResolveActiveModeDriver() const;
    // Establish map-scoped role authority exactly once when a loaded map's
    // active mode is installed. This is intentionally not a round-reset hook:
    // retail squad membership survives ordinary rounds but never map changes.
    void ActivateRoleAuthorityForMap(const std::string& mapName);
    void InitializeActiveModeDriver();
    void RequestNativeModeMapChangeIfFinished();
    void ObserveNativeModeCompletion(bool nativeDriverOwnsRound,
                                     bool nativeModePresent,
                                     bool finished);
    void ResetNativeModeMapChangeLatch();
    bool ConsumeDeferredMapChange();
    // Rebuild the optional generic RoundManager/GameState pair after a map's
    // active driver and objectives have been selected. Native modes always
    // discard these central objects so there is only one round authority.
    void ReconcileCentralRoundState();
    // Limit ObjectiveSystem simulation to the portion of this frame that
    // remains inside the current native mode's capturable phase.
    float GetObjectiveCaptureDelta(float deltaSeconds) const;

    // Register the current map's objective definitions with the ObjectiveSystem,
    // applying linear territory ordering when the active mode is Territory.
    void PopulateObjectivesFromMap();
    // Feed the loaded map's spawn points into the SpawnSystem so joined players
    // can actually spawn (otherwise SpawnSystem is empty -> "no spawn locations").
    void PopulateSpawnsFromMap();
    void InitializeBotsForCurrentMap();
    void RefreshBotWorldState();
    bool IsBotGameplayLive() const;
    void SynchronizeCombatParticipants();
    void SynchronizeCombatParticipant(uint32_t clientId, bool forceRespawn);
    // Releases cooks whose server-owned fuse has expired even if a client
    // never sends h30 (disconnect/loss/held grenade). The spawned projectile
    // has only the small minimum fuse left, producing an in-hand detonation.
    void AdvanceRetailGrenadeCooks();
    void ReplicateRetailM61Projectiles(float deltaSeconds);
    void ProcessCombatEvents(
        const std::vector<CombatAuthority::CombatEvent>& events);
    // Applies one server-issued bot shot to CombatAuthority's connected-human
    // state and mirrors the result. Returns true only when bot score changed.
    bool ProcessBotHumanCombatEvent(const BotHumanCombatEvent& event);
    // Publishes one committed BotManager death transaction. Ticket pools are
    // debited as a batch before any native-mode callback can choose a winner.
    void ProcessBotDeathBatch(const std::vector<BotDeathEvent>& deaths);

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
    // Explicit admin target. This has priority over voting/automatic rotation
    // and is consumed exactly once by the next deferred ChangeMap().
    std::string                         m_pendingRequestedMap;
    bool                                m_mapChangeRequested = false;
    // A terminal native driver gets one rotation attempt. Keep this latched if
    // ChangeMap cannot find/load a successor so Finished does not hot-loop.
    bool                                m_nativeModeMapChangeLatched = false;
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
    std::unique_ptr<CombatAuthority::Authority> m_combatAuthority;
    std::unique_ptr<MantleAuthority::Authority> m_mantleAuthority;
    struct RetailGrenadeCookState {
        enum class Variant : uint8_t { M61, Type67 };
        uint8_t fireMode = 0;
        double beganAtAuthoritySeconds = 0.0;
        Variant variant = Variant::M61;
    };
    std::unordered_map<uint32_t, RetailGrenadeCookState> m_retailGrenadeCooks;
    std::unordered_set<uint64_t> m_retailM61Projectiles;
    float m_retailM61ReplicationAccumulator = 0.0f;
    std::unique_ptr<ProjectileManager>  m_projectileManager;
    std::unique_ptr<HelicopterPhysics>  m_helicopterPhysics;
    std::unique_ptr<BotManager>         m_botManager;
    std::uint64_t                       m_botCombatGeneration = 0;
    std::vector<uint32_t>               m_botObjectiveSignature;
    int                                 m_botTerritoryRedeployedRound = -1;
    int                                 m_botTerritoryDeploymentPhase = -1;
    std::uint32_t                       m_botTerritoryAttackingTeam = 0;
    std::unique_ptr<TerritoryMode>      m_territoryMode;
    std::unique_ptr<SupremacyMode>      m_supremacyMode;
    std::unique_ptr<SkirmishMode>       m_skirmishMode;
    ActiveModeDriver                    m_activeModeDriver{ActiveModeDriver::Generic};

    // Central round/game-state layer. Opt-in (Game.use_round_manager): when
    // enabled, RoundManager drives the generic Preparation/Active/PostRound
    // cycle over GameState. Off by default so the per-mode classes above remain
    // the sole round drivers.
    std::unique_ptr<GameState>          m_gameState;
    std::unique_ptr<RoundManager>       m_roundManager;

    // Replication + the connection->player login bridge (the bridge owns the
    // SecurityManager internally; see GameServer::Initialize for why).
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

    // Per-client last weapon-fire timestamp (ms, steady_clock) for the server-side rate-of-fire
    // gate in HandleWeaponFire. Partial of the "no server-side firing authority" finding; full
    // per-player Weapon-instance ammo/reload wiring is a follow-up.
    std::unordered_map<uint32_t, uint64_t> m_lastWeaponFireMs;

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
