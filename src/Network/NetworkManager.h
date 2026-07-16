// src/Network/NetworkManager.h

#pragma once

#include <memory>
#include <vector>
#include "Network/BandwidthManager.h"
#include "Network/Packet.h"
#include "Network/HandshakeState.h"  // ClientLoggedIn/Joined event + callback types
#include "Network/ClientTravelReplication.h"
#include "Game/ParticipantRoster.h"
#include "Math/Vector3.h"

class GameServer;
class ConnectionManager;
class ClientConnection;
namespace WeaponCombatRepl {
struct M61VisualSnapshot;
}

class NetworkManager {
public:
    explicit NetworkManager(GameServer* server);
    ~NetworkManager();

    bool Initialize(uint16_t listenPort);
    void Shutdown();

    // Poll network I/O (call once per tick from GameServer::Run)
    void PollNetwork();

    // Flush any buffered outgoing data
    void Flush();

    bool SendPacket(uint32_t clientId, const Packet& pkt);
    void BroadcastPacket(const Packet& pkt);
    void BroadcastPacket(const std::string& tag, const std::vector<uint8_t>& data);

    // Retail UE3 path for ROGameReplicationInfo objective arrays. Unlike the
    // legacy OBJECTIVE_UPDATE packet, this is actor-channel replication.
    void BroadcastRetailObjectiveState();

    // Reliable UE3 map-rotation seam. The RPC is pre-encoded so GameServer can
    // validate it before committing the local map load.
    size_t BroadcastRetailClientTravel(
        const ClientTravelRepl::EncodedRpc& rpc,
        const std::string& mapUrl);
    bool CanBroadcastRetailClientTravel(
        const ClientTravelRepl::EncodedRpc& rpc,
        const std::string& mapUrl,
        size_t* eligibleClients = nullptr) const;

    // Advance the retail deployment/countdown RPC state after the native game
    // mode clock has ticked for this frame.
    void UpdateRetailDeploymentCountdown();
    bool ShouldAdvanceRetailRoundClock() const;

    void ReplicateRetailCombatState(uint32_t clientId, int health, int kills,
                                    int deaths, int score, bool isDead,
                                    bool sendHealth, bool sendDeathRpc);
    bool ResetRetailMovementValidation(
        uint32_t clientId, const Vector3& authoritativePosition);
    void ReplicateRetailParticipantCombatState(
        const ParticipantId& participant, int health, int kills, int deaths,
        int score, bool isDead, bool sendHealth, bool sendDeathRpc);
    void RemoveRetailParticipant(const ParticipantId& participant);
    void BroadcastRetailM61Spawn(
        uint64_t projectileKey, uint32_t shooterClientId,
        const WeaponCombatRepl::M61VisualSnapshot& snapshot);
    void BroadcastRetailM61Update(
        uint64_t projectileKey,
        const WeaponCombatRepl::M61VisualSnapshot& snapshot);
    void BroadcastRetailM61Detonate(uint64_t projectileKey,
                                    float fuseSeconds);
    void BroadcastRetailM61Remove(uint64_t projectileKey);

    uint32_t GetClientId(const ClientAddress& addr) const;
    uint32_t FindClientBySteamID(const std::string& steamId) const;
    std::shared_ptr<ClientConnection> GetConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;

    uint32_t GetBandwidthLimit() const;
    int GetPacketsPerSecond() const;

    // ---- Game-facing handshake observer subscription (Stream B hooks here) --
    // These forward to ConnectionManager so the Game layer can react to the
    // control-channel handshake without depending on Network internals.
    void SetClientLoggedInCallback(ClientLoggedInCallback cb);
    void SetClientJoinedCallback(ClientJoinedCallback cb);

private:
    GameServer*                                m_server;
    std::unique_ptr<ConnectionManager>         m_connMgr;
    std::unique_ptr<BandwidthManager>          m_bwManager;
    uint32_t                                   m_bandwidthLimit = 65536;
    mutable int                                m_packetsThisTick = 0;

    void OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);
};
