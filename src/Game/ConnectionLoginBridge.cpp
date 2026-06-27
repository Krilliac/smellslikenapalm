// src/Game/ConnectionLoginBridge.cpp
//
// See ConnectionLoginBridge.h. Mirrors the SERVER-side UE3 login flow
// (Engine/GameInfo.uc PreLogin/Login/PostLogin + Engine/AccessControl.uc) onto
// the control-channel ClientLoggedIn / ClientJoined events.

#include "Game/ConnectionLoginBridge.h"

#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/SpawnSystem.h"
#include "Game/Player.h"
#include "Security/SecurityManager.h"
#include "Config/SecurityConfig.h"
#include "Protocol/ReplicationManager.h"
#include "Protocol/PropertyReplication.h"
#include "Network/ClientConnection.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"

#include <cstring>

ConnectionLoginBridge::ConnectionLoginBridge(Dependencies deps)
    : m_deps(std::move(deps))
{
    // Resolve the SecurityManager: prefer an externally-provided one, otherwise
    // construct our own from the SecurityConfig (so the PreLogin ban gate is
    // always available without GameServer having to include the Security headers
    // — which would collide with the duplicate ClientAddress in BandwidthManager).
    if (m_deps.securityManager) {
        m_security = m_deps.securityManager;
    } else if (m_deps.securityConfig) {
        m_ownedSecurity = std::make_unique<SecurityManager>(m_deps.securityConfig);
        if (!m_ownedSecurity->Initialize()) {
            Logger::Warn("[LoginBridge] owned SecurityManager init failed — continuing with degraded security");
        }
        m_security = m_ownedSecurity.get();
    }
    Logger::Info("ConnectionLoginBridge constructed (the connection->player bridge is now live)");
}

ConnectionLoginBridge::~ConnectionLoginBridge()
{
    if (m_ownedSecurity) {
        m_ownedSecurity->Shutdown();
    }
}

// ===========================================================================
//  ClientLoggedIn  ==  GameInfo::PreLogin + GameInfo::Login
// ===========================================================================
void ConnectionLoginBridge::OnClientLoggedIn(const ClientLoggedInEvent& ev)
{
    Logger::Info("[LoginBridge] ClientLoggedIn: client=%u steam=0x%llx name='%s' team=%d",
                 ev.clientId, (unsigned long long)ev.steamId,
                 ev.options.PlayerName().c_str(), ev.options.Team());

    // Resolve the live connection (may be null in pure unit tests that drive the
    // promotion through a fabricated connection instead).
    std::shared_ptr<ClientConnection> conn =
        m_deps.resolveConnection ? m_deps.resolveConnection(ev.clientId) : nullptr;

    // --- PreLogin gate (Engine/AccessControl.uc): capacity / password / ban ---
    std::string reason;
    if (!PreLogin(ev, conn, reason)) {
        Logger::Warn("[LoginBridge] PreLogin REJECTED client=%u: %s", ev.clientId, reason.c_str());
        if (m_deps.dropConnection) {
            m_deps.dropConnection(ev.clientId, reason);
        }
        return;   // No player is created — mirrors Login() returning none.
    }

    if (!conn) {
        // Nothing to promote without a connection; the dead PlayerManager::
        // OnPlayerConnect path is connection-driven. This is not an error in
        // production (the resolver always returns a connection) but keeps the
        // bridge defensive.
        Logger::Warn("[LoginBridge] client=%u passed PreLogin but has no resolvable connection; cannot promote",
                     ev.clientId);
        return;
    }

    // --- GameInfo::Login: parse Name/Team and build the player identity -------
    // ParseOption("Name"); empty -> DefaultPlayerName + PlayerID (assigned below).
    std::string inName = ev.options.PlayerName();
    int inTeam = ev.options.Team();                 // GetIntOption("Team", 255)
    bool spectator = ev.options.SpectatorOnly();

    // PickTeam (Engine/GameInfo::PickTeam, called from Login). We honour the
    // requested team when valid; otherwise leave 255 ("no preference") for the
    // join-time team pick. RS2 teams are 0/1 (US/NVA); accept either 0/1 or the
    // 1/2 ids TeamManager uses, leaving everything else as "no preference".
    int pickedTeam = inTeam;

    // Stamp the connection with the resolved identity BEFORE promotion: the dead
    // PlayerManager::OnPlayerConnect reads name/team straight off the connection
    // (conn->GetPlayerName()/GetTeamId()). This is the wiring that was missing.
    conn->SetPlayerName(inName);
    conn->SetTeamId(pickedTeam == 255 ? 0u : (uint32_t)pickedTeam);
    // Stamp the authenticated Steam64 so admin/ban lookups (keyed on Steam64) can match.
    // Only when a real id was resolved (non-zero); RS2's minimal Hello often omits it, in
    // which case GetSteamID keeps falling back to the clientId string (no behavior change).
    if (ev.steamId != 0) {
        conn->SetSteamID(std::to_string(ev.steamId));
    }

    // *** WIRE UP the dead PlayerManager::OnPlayerConnect ***
    // (src/Game/PlayerManager.cpp:33 — previously had no caller). This is the
    // connection->Player promotion: it creates the Player keyed by clientId.
    if (!m_deps.playerManager) {
        Logger::Error("[LoginBridge] No PlayerManager — cannot promote client %u", ev.clientId);
        return;
    }
    m_deps.playerManager->OnPlayerConnect(conn);

    auto player = m_deps.playerManager->GetPlayer(ev.clientId);
    if (!player) {
        Logger::Error("[LoginBridge] PlayerManager::OnPlayerConnect did not yield a Player for client %u",
                      ev.clientId);
        return;
    }

    // Allocate the monotonic PlayerID + UniqueId (GameInfo::Login steps 14/15:
    // PlayerID = GetNextPlayerID(); PRI.SetUniqueId(UniqueId)).
    PlayerReplicationInfo pri;
    pri.clientId     = ev.clientId;
    pri.playerId     = m_nextPlayerId++;
    pri.uniqueId     = ev.steamId;                  // Steam id == UniqueNetId
    pri.bBot         = false;
    pri.team         = pickedTeam;
    pri.bIsSpectator = spectator;
    pri.score        = 0;
    pri.deaths       = 0;

    // ChangeName: default name if the client supplied none
    // (GameInfo::Login step 18: InName = DefaultPlayerName $ PlayerID).
    pri.playerName = inName.empty() ? ("Player" + std::to_string(pri.playerId)) : inName;
    if (inName.empty()) {
        conn->SetPlayerName(pri.playerName);
    }

    m_playerInfos[ev.clientId] = pri;

    // --- Initial replication: ensure a single GRI, register this PRI ----------
    EnsureGameReplicationInfo();
    RegisterReplicatedPRI(m_playerInfos[ev.clientId]);

    Logger::Info("[LoginBridge] Login complete: client=%u -> Player '%s' PlayerID=%d team=%d uniqueId=0x%llx (PRI actor %u)",
                 ev.clientId, pri.playerName.c_str(), pri.playerId, pri.team,
                 (unsigned long long)pri.uniqueId, m_playerInfos[ev.clientId].actorId);
}

// ===========================================================================
//  ClientJoined  ==  GameInfo::PostLogin (RestartPlayer / spawn)
// ===========================================================================
void ConnectionLoginBridge::OnClientJoined(const ClientJoinedEvent& ev)
{
    Logger::Info("[LoginBridge] ClientJoined: client=%u", ev.clientId);

    auto it = m_playerInfos.find(ev.clientId);
    if (it == m_playerInfos.end()) {
        Logger::Warn("[LoginBridge] ClientJoined for client %u with no prior login — ignoring", ev.clientId);
        return;
    }
    PlayerReplicationInfo& pri = it->second;

    auto player = m_deps.playerManager ? m_deps.playerManager->GetPlayer(ev.clientId) : nullptr;
    if (!player) {
        Logger::Warn("[LoginBridge] ClientJoined: no Player for client %u", ev.clientId);
        return;
    }

    // PickTeam (final): if the client expressed no preference (255), pick the
    // smaller team via TeamManager; otherwise honour the request. RS2 team ids
    // 0/1 map onto TeamManager's 1/2.
    uint32_t finalTeam;
    if (pri.team == 255 || pri.team < 0) {
        if (m_deps.teamManager) {
            uint32_t t1 = (uint32_t)m_deps.teamManager->GetTeamSize(1);
            uint32_t t2 = (uint32_t)m_deps.teamManager->GetTeamSize(2);
            finalTeam = (t1 <= t2) ? 1u : 2u;
        } else {
            finalTeam = 1u;
        }
    } else {
        // Map RS2/UE3 team ids 0/1 onto TeamManager 1/2. (Was passing 1 through unchanged,
        // landing UE3 team 1 / NVA on TeamManager team 1 / US Army - the wrong faction.)
        finalTeam = (pri.team == 0) ? 1u : 2u;
    }
    pri.team = (int32_t)finalTeam;

    if (m_deps.teamManager) {
        m_deps.teamManager->AddPlayerToTeam(ev.clientId, finalTeam);   // also sets Player team
    } else {
        player->SetTeam(finalTeam);
    }

    // RestartPlayer: FindPlayerStart + spawn via the existing SpawnSystem.
    bool spawned = false;
    if (!pri.bIsSpectator && m_deps.spawnSystem) {
        spawned = m_deps.spawnSystem->SpawnPlayerAtDefault(ev.clientId);
        if (!spawned) {
            // No spawn locations registered yet (e.g. map not loaded). Still mark
            // the player active so the join completes; an actual pawn will follow
            // a later SPAWN_REQUEST. TODO: tie into FindPlayerStart once map
            // PlayerStart actors are loaded for the current map.
            Logger::Warn("[LoginBridge] No spawn available for client %u; marking active without pawn", ev.clientId);
            m_deps.playerManager->OnPlayerSpawn(ev.clientId);
        }
    } else if (pri.bIsSpectator) {
        Logger::Info("[LoginBridge] client %u joined as spectator — no spawn", ev.clientId);
        player->SetState(PlayerState::Spectating);
    } else {
        // No SpawnSystem wired; still mark active.
        m_deps.playerManager->OnPlayerSpawn(ev.clientId);
    }

    m_spawnAttempted[ev.clientId] = true;

    // Re-queue the PRI now that team/score are finalized (bNetDirty group).
    RegisterReplicatedPRI(pri);

    Logger::Info("[LoginBridge] Join complete: client=%u team=%u spawned=%s state=%d",
                 ev.clientId, finalTeam, spawned ? "true" : "false",
                 (int)player->GetState());
}

// ===========================================================================
//  PreLogin gate
// ===========================================================================
bool ConnectionLoginBridge::PreLogin(const ClientLoggedInEvent& ev,
                                     const std::shared_ptr<ClientConnection>& conn,
                                     std::string& outReason)
{
    bool spectator = ev.options.SpectatorOnly();

    // --- Capacity (AccessControl.PreLogin step 2: AtCapacity) ----------------
    // Player slots come from ServerConfig.MaxPlayers. Spectators are not gated
    // here yet (RS2 tracks them separately); EAC/VAC/queue are no-op success
    // stubs per the roadmap.
    if (m_deps.serverConfig && m_deps.playerManager && !spectator) {
        int maxPlayers = m_deps.serverConfig->GetMaxPlayers();
        if (maxPlayers > 0) {
            int current = (int)m_deps.playerManager->GetAllPlayers().size();
            if (current >= maxPlayers) {
                outReason = "Engine.GameMessage.MaxedOutMessage";   // server full
                return false;
            }
        }
    }

    // --- Password (AccessControl.PreLogin step 3) ----------------------------
    // GamePassword from config (key "Security.game_password" / "Server.Password").
    // Empty configured password == no password required.
    std::string configuredPassword;
    if (m_deps.serverConfig) {
        auto mgr = m_deps.serverConfig->GetManager();
        if (mgr) {
            configuredPassword = mgr->GetString("Server.password", "");
            if (configuredPassword.empty()) {
                configuredPassword = mgr->GetString("Security.game_password", "");
            }
        }
    }
    if (!configuredPassword.empty()) {
        std::string supplied = ev.options.Password();
        if (supplied != configuredPassword) {
            outReason = supplied.empty() ? "Engine.AccessControl.NeedPassword"
                                         : "Engine.AccessControl.WrongPassword";
            return false;
        }
    }

    // --- ID / IP ban (GameInfo::Login IsIDBanned + AccessControl IP policy) ---
    // *** WIRE UP the unwired SecurityManager::OnClientConnect ***
    // (src/Security/SecurityManager.cpp:76 — previously never called). It tracks
    // the connection, runs the ban gate, kicks banned SteamIDs, and kicks off
    // auth/EAC validation. We treat a connection that survives it (still not
    // disconnected) as having passed the ID/IP ban gate.
    if (m_security && conn) {
        m_security->OnClientConnect(conn);
        if (conn->IsDisconnected()) {
            outReason = "Engine.AccessControl.IDBanned";   // SecurityManager kicked it
            return false;
        }
    }

    // Steam/EOS auth already accepted by Stream A-int — do not re-validate.
    return true;
}

// ===========================================================================
//  Replication helpers (use the EXISTING ReplicationManager)
// ===========================================================================
void ConnectionLoginBridge::EnsureGameReplicationInfo()
{
    if (m_gri) return;   // single GRI per server, created lazily

    m_gri = std::make_unique<GameReplicationInfo>();
    if (m_deps.serverConfig) {
        m_gri->serverName = m_deps.serverConfig->GetServerName();
    }
    if (m_gri->serverName.empty()) {
        m_gri->serverName = "Rising Storm 2: Vietnam Server";   // Engine default
    }
    m_gri->gameClass = "ROGame.ROGameInfo";

    if (m_deps.replicationManager) {
        m_gri->actorId = m_nextActorId++;
        m_deps.replicationManager->RegisterActor(m_gri->actorId);

        // Queue the initial GRI state (bNetInitial: ServerName/GameClass/times)
        // as a custom-property update so it replicates on the next tick.
        PropertyState ps{};
        ps.objectId = m_gri->actorId;
        ps.flags    = PR_CUSTOM;
        ps.customData.assign(m_gri->serverName.begin(), m_gri->serverName.end());
        m_deps.replicationManager->QueuePropertyUpdate(ps);
        m_deps.replicationManager->MarkActorDirty(m_gri->actorId, PR_CUSTOM);
    }

    Logger::Info("[LoginBridge] GameReplicationInfo ready: server='%s' gameClass='%s' (actor %u)",
                 m_gri->serverName.c_str(), m_gri->gameClass.c_str(), m_gri->actorId);
}

void ConnectionLoginBridge::RegisterReplicatedPRI(PlayerReplicationInfo& pri)
{
    if (!m_deps.replicationManager) {
        Logger::Debug("[LoginBridge] No ReplicationManager — PRI for client %u not replicated", pri.clientId);
        return;
    }

    if (pri.actorId == 0) {
        pri.actorId = m_nextActorId++;
        m_deps.replicationManager->RegisterActor(pri.actorId);
    }

    // Pack the initial PRI fields (PlayerID, Team, bBot, UniqueId, name) into a
    // custom property payload. The exact UE3 bunch layout is pinned elsewhere;
    // here we just hand the ReplicationManager a registered, dirty actor so the
    // PRI participates in replication.
    PropertyState ps{};
    ps.objectId = pri.actorId;
    ps.flags    = PR_CUSTOM | PR_STATE;
    ps.state    = (uint32_t)pri.team;

    auto& cd = ps.customData;
    auto pushU32 = [&cd](uint32_t v) {
        for (int i = 0; i < 4; ++i) cd.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
    };
    auto pushU64 = [&cd](uint64_t v) {
        for (int i = 0; i < 8; ++i) cd.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
    };
    pushU32((uint32_t)pri.playerId);
    pushU32((uint32_t)pri.team);
    cd.push_back(pri.bBot ? 1 : 0);
    cd.push_back(pri.bIsSpectator ? 1 : 0);
    pushU64(pri.uniqueId);
    pushU32((uint32_t)pri.playerName.size());
    cd.insert(cd.end(), pri.playerName.begin(), pri.playerName.end());

    m_deps.replicationManager->QueuePropertyUpdate(ps);
    m_deps.replicationManager->MarkActorDirty(pri.actorId, PR_CUSTOM | PR_STATE);

    Logger::Debug("[LoginBridge] PRI registered/updated for client %u (actor %u, PlayerID %d, team %d)",
                  pri.clientId, pri.actorId, pri.playerId, pri.team);
}

// ===========================================================================
//  Accessors
// ===========================================================================
const PlayerReplicationInfo* ConnectionLoginBridge::GetPlayerInfo(uint32_t clientId) const
{
    auto it = m_playerInfos.find(clientId);
    return it != m_playerInfos.end() ? &it->second : nullptr;
}

bool ConnectionLoginBridge::WasSpawnAttempted(uint32_t clientId) const
{
    auto it = m_spawnAttempted.find(clientId);
    return it != m_spawnAttempted.end() && it->second;
}
