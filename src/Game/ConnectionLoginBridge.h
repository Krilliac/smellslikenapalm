// src/Game/ConnectionLoginBridge.h
//
// THE missing bridge between the Network control-channel handshake (Stream
// A-int) and the Game layer (Stream B). It turns a connected client into a
// spawned player by mirroring the SERVER-side UE3 login flow:
//
//   ClientLoggedIn (after NMT_Login / Welcome sent)
//        -> PreLogin gate  (Engine/AccessControl.uc: capacity / password / ban)
//        -> Login          (Engine/GameInfo.uc::Login: parse Name/Team, make PRI)
//        -> connection->Player promotion (wires PlayerManager::OnPlayerConnect)
//        -> initial replication (GRI ensured + per-player PRI registered)
//
//   ClientJoined (after NMT_Join)
//        -> PostLogin      (Engine/GameInfo.uc::PostLogin: PickTeam, spawn,
//                           mark active)
//
// This class OWNS no networking. It reaches the Network layer only through a
// connection-resolver callback (so it is trivially unit-testable without a live
// socket) and subscribes to NetworkManager's Set*Callback events at the call
// site (GameServer).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "Network/HandshakeState.h"   // ClientLoggedInEvent / ClientJoinedEvent
#include "Game/ReplicationInfo.h"

class PlayerManager;
class TeamManager;
class SpawnSystem;
class SecurityManager;
class SecurityConfig;
class ReplicationManager;
class ClientConnection;
class ServerConfig;

class ConnectionLoginBridge {
public:
    // Resolves a clientId to its live ClientConnection (GameServer wires this to
    // GameServer::GetClientConnection; a unit test wires its own).
    using ConnectionResolver =
        std::function<std::shared_ptr<ClientConnection>(uint32_t clientId)>;

    // Drops/kicks a connection that failed PreLogin (GameServer wires this to a
    // best-effort disconnect; may be null).
    using DropConnectionFn =
        std::function<void(uint32_t clientId, const std::string& reason)>;

    struct Dependencies {
        PlayerManager*      playerManager     = nullptr;
        TeamManager*        teamManager       = nullptr;   // may be null
        SpawnSystem*        spawnSystem       = nullptr;   // may be null
        SecurityManager*    securityManager   = nullptr;   // may be null; if null
                                                           // and securityConfig is
                                                           // set, the bridge owns one
        std::shared_ptr<SecurityConfig> securityConfig;    // may be null
        ReplicationManager* replicationManager = nullptr;  // may be null
        std::shared_ptr<ServerConfig> serverConfig;        // may be null
        ConnectionResolver  resolveConnection;
        DropConnectionFn    dropConnection;                // may be null
    };

    explicit ConnectionLoginBridge(Dependencies deps);
    ~ConnectionLoginBridge();

    // ---- Handshake event handlers (wired to NetworkManager Set*Callback) -----

    // Fired when a client completes NMT_Login. Runs PreLogin gate, then Login
    // (promotes the connection to a Player) and initial replication.
    void OnClientLoggedIn(const ClientLoggedInEvent& ev);

    // Fired when a client sends NMT_Join. Runs PostLogin: team pick + spawn +
    // mark active.
    void OnClientJoined(const ClientJoinedEvent& ev);

    // ---- Test / introspection accessors -------------------------------------

    // The PRI for a logged-in client, or nullptr if no player was created.
    const PlayerReplicationInfo* GetPlayerInfo(uint32_t clientId) const;

    // The single GameReplicationInfo (created lazily on first login).
    const GameReplicationInfo* GetGameInfo() const {
        return m_gri ? m_gri.get() : nullptr;
    }

    // The authoritative SecurityManager (owned-or-injected) that runs the ban gate at
    // connect; nullptr if security was not configured. Lets the admin layer route /ban
    // through the single ENFORCED ban store rather than a parallel one.
    SecurityManager* GetSecurityManager() const { return m_security; }

    // True once OnClientJoined successfully attempted a spawn for the client.
    bool WasSpawnAttempted(uint32_t clientId) const;

private:
    // PreLogin gate (Engine/AccessControl.uc). Returns true to accept. On
    // rejection sets outReason. Uses SecurityManager::OnClientConnect for the
    // ban/IP gate when available.
    bool PreLogin(const ClientLoggedInEvent& ev,
                  const std::shared_ptr<ClientConnection>& conn,
                  std::string& outReason);

    // Ensure the single GRI exists and is registered with the replication
    // manager. Idempotent.
    void EnsureGameReplicationInfo();

    // Register a PRI as a replicated actor and queue its initial state.
    void RegisterReplicatedPRI(PlayerReplicationInfo& pri);

    Dependencies m_deps;

    // SecurityManager the bridge constructed itself (when deps.securityManager
    // was null but deps.securityConfig was provided). m_security points at
    // either this owned instance or deps.securityManager.
    std::unique_ptr<SecurityManager> m_ownedSecurity;
    SecurityManager*                 m_security = nullptr;

    std::unique_ptr<GameReplicationInfo> m_gri;

    // Per-client PRI state, keyed by network clientId.
    std::unordered_map<uint32_t, PlayerReplicationInfo> m_playerInfos;

    // Clients for which a spawn was attempted in OnClientJoined.
    std::unordered_map<uint32_t, bool> m_spawnAttempted;

    // Monotonic PlayerID allocator (UE3 PlayerReplicationInfo.PlayerID).
    int32_t  m_nextPlayerId = 0;

    // Distinct, monotonic replication actor id allocator. Kept above the range
    // network clientIds occupy so PRI/GRI actor ids never collide with them.
    uint32_t m_nextActorId = 0x20000000u;
};
