// src/Network/ConnectionManager.h

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include "Network/ClientConnection.h"
#include "Network/UDPSocket.h"
#include "Network/BandwidthManager.h"
#include "Network/HandshakeState.h"
#include "Network/PacketCodec.h"
#include "Network/PacketAssembler.h"
#include "Network/ControlReassembler.h"

class GameServer;

class ConnectionManager {
public:
    using PacketCallback = std::function<void(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta)>;

    explicit ConnectionManager(GameServer* server);
    ~ConnectionManager();

    // Initialize networking subsystems
    bool Initialize(uint16_t listenPort);
    void Shutdown();

    // Set callback for received packets (used by NetworkManager)
    void SetPacketCallback(PacketCallback cb);

    // ---- Game-facing handshake observer interface --------------------------
    // The Game layer (Stream B) subscribes to these to react to control-channel
    // handshake progress WITHOUT the Network layer depending on Game/. The
    // handshake state machine calls FireClientLoggedIn / FireClientJoined.
    void SetClientLoggedInCallback(ClientLoggedInCallback cb);
    void SetClientJoinedCallback(ClientJoinedCallback cb);
    void FireClientLoggedIn(const ClientLoggedInEvent& ev);
    void FireClientJoined(const ClientJoinedEvent& ev);

    // Send raw control-channel bytes (a ControlChannel::Build* payload) to a
    // client without the Packet tag/serialize wrapper. Used by HandshakeState.
    bool SendRawToClient(uint32_t clientId, const std::vector<uint8_t>& bytes);

    // Main loop: receive raw data and dispatch to handlers
    void PumpNetwork();

    // Send utilities
    bool SendToClient(uint32_t clientId, const Packet& pkt);
    void Broadcast(const Packet& pkt);

    // Client management
    std::shared_ptr<ClientConnection> GetConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;
    uint32_t FindClientByAddress(const std::string& ip, uint16_t port) const;
    uint32_t FindClientBySteamID(const std::string& steamId) const;
    uint32_t CreateOrGetClient(const std::string& ip, uint16_t port);

    // Periodic housekeeping
    void RemoveStaleConnections();
    void UpdateBandwidthWindows();

    // Bandwidth limit query (used by ClientConnection)
    uint32_t GetBandwidthLimit() const;

    // Configuration
    void SetMaxClients(size_t maxClients);
    size_t GetMaxClients() const;

private:
    GameServer* m_server;
    std::shared_ptr<UDPSocket> m_socket;
    PacketCallback m_packetCallback;

    // Game-facing handshake observers (set by Stream B; null = not subscribed).
    ClientLoggedInCallback m_clientLoggedInCb;
    ClientJoinedCallback   m_clientJoinedCb;

    // Client lookup by address
    std::unordered_map<ClientAddress, std::shared_ptr<ClientConnection>> m_clients;

    // Per-connection control-channel handshake state machines, keyed by clientId.
    std::unordered_map<uint32_t, std::unique_ptr<HandshakeState>> m_handshakes;

    // Per-connection UE3 control-channel framing state, keyed by clientId:
    //   * outbound    - assigns PacketId/ChSequence, fragments + acks (send side).
    //   * reassembler - orders/dedups inbound reliable control bunches and peels
    //                   complete messages into the handshake (receive side).
    struct ControlState {
        PacketCodec::PacketAssembler outbound;
        std::unique_ptr<PacketCodec::ControlReassembler> reassembler;
        // Per-channel state for the owning client's PlayerController (ch2). The actor
        // bootstrap opens ch2 (seq 1) then sends ClientShowTeamSelect (seq 2); each
        // further server->client reliable RPC on ch2 (ClientShowRoleSelect, ...)
        // increments ch2OutReliable. actorChType is ch2's ChType (CHTYPE_Actor) reused
        // for those bunches. teamSelected guards the SelectTeam->role-select advance.
        uint32_t ch2OutReliable = 0;
        uint32_t actorChType = 2;
        bool     teamSelected = false;
        bool     menuResent = false;   // re-sent ClientShowTeamSelect on client proof-of-life

        // ---- Reliable retransmission (UE3 UNetConnection-style) -----------------
        // UE3 reliability: a reliable bunch must be re-sent until the client acks the
        // packet that carried it. Without this, one dropped reliable bunch in the
        // bootstrap burst stalls that channel forever -> the client soft-locks (can't
        // disconnect). We record each reliable bunch-set we send, clear it when the
        // client acks any packet it rode in, and resend (verbatim, SAME per-channel
        // ChSequence; NEW PacketId) on an ack-timeout.
        struct SentReliable {
            std::vector<uint32_t> packetIds;     // every packet this set has gone out in
            uint64_t lastSendMs = 0;
            int      resendCount = 0;
            std::vector<PacketCodec::Bunch> bunches;  // the reliable bunches, verbatim
        };
        std::vector<SentReliable> pendingReliable;
    };
    std::unordered_map<uint32_t, ControlState> m_controlState;
    uint32_t m_nextClientId{1};
    size_t m_maxClients{256};
    uint32_t m_bandwidthLimit{65536};

    // Bandwidth manager
    std::unique_ptr<BandwidthManager> m_bwManager;

    // Helpers
    void HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr);

    // Get-or-create the handshake state machine for a client.
    HandshakeState& GetOrCreateHandshake(uint32_t clientId);

    // Get-or-create the per-connection control-channel framing state (lazily wires
    // the reassembler's message callback to the client's handshake).
    ControlState& GetControlState(uint32_t clientId);

    // Frame + encode an outbound control packet and push it to the client.
    void SendEncodedPacket(uint32_t clientId, const PacketCodec::Packet& pkt);

    // Post-Join WORLD REPLICATION bootstrap. Once a client reaches Joined the
    // retail client sits on the loading screen until the server replicates the
    // world - first the PackageMap export (the map's package/NetGUID list), then
    // the bootstrap actor channels. We replay the official server's recorded
    // post-Welcome control-channel messages (reversed from the handshake capture;
    // see docs/RS2V_PostJoin_Replication_7258.md): a sequence of complete reliable
    // control bunches (NMT 0x07 PackageMap chunks, ...), loaded from
    // data/replication_bootstrap.bin (a stream of [uint32 LE len][payload] records)
    // and sent in order via SendRawToClient. No-op (logged) if the file is absent.
    void SendReplicationBootstrap(uint32_t clientId);

    // Send a reliable server->client function-call bunch on the PlayerController
    // channel (ch2): payload = SerializeInt(handle, maxHandle) + any params, already
    // packed into `payload`/`payloadBits` by the caller. Assigns the next ch2 reliable
    // ChSequence. Used for ClientShowTeamSelect / ClientShowRoleSelect / ChangedTeams.
    void SendCh2Rpc(uint32_t clientId, const std::vector<uint8_t>& payload,
                    uint32_t payloadBits, const char* name);

    // Replicate the local PlayerController's PlayerReplicationInfo (Controller handle 23)
    // as an UNRELIABLE ch2 object-ref to the local PRI channel (ch26) - the exact bytes
    // the real server streams ("176a00"). Without this link the client's
    // ROPC.PlayerReplicationInfo is none and the role/unit-select UI derefs null
    // (VNGame.exe+0xbbf712). Sent `repeats` times for unreliable delivery.
    void SendLocalPriLink(uint32_t clientId, int repeats);

    // Clear the local PRI's spectator/waiting flags on ch26 (bWaitingPlayer h31,
    // bOnlySpectator h32, bIsSpectator h33 -> 0) as an UNRELIABLE property delta, so
    // ShowRoleSelectScene does not early-return at ROPlayerController.uc:5932
    // (if PlayerReplicationInfo.bOnlySpectator return). Sent `repeats` times.
    void SendClearSpectator(uint32_t clientId, int repeats);

    // Decode one inbound actor-channel (ChIndex>=2) bunch: read the field handle
    // (SerializeInt at the PlayerController maxHandle) and dispatch the client->server
    // RPC (e.g. SelectTeam). Logs the handle/name for visibility into client input.
    void DecodeInboundActorBunch(uint32_t clientId, const PacketCodec::Bunch& bunch);

    // ---- Reliable retransmission ------------------------------------------------
    // Build ONE packet from `bunches`, send it, and record any reliable bunches for
    // retransmission until acked. The single choke-point for sending actor bunches.
    void SendReliableBunches(uint32_t clientId, const std::vector<PacketCodec::Bunch>& bunches);
    // The client acked `ackedPacketId`: drop any pending reliable set that rode in it.
    void OnClientAck(uint32_t clientId, uint32_t ackedPacketId);
    // Per-poll: resend reliable bunch-sets the client hasn't acked within the timeout.
    void RetransmitTick();

    // Open the bootstrap ACTOR channels after the client confirms Join. Replays the
    // official server's f231 burst (ROGameReplicationInfo on ch2, TeamInfo, the
    // local ROPlayerController on ch6, PlayerReplicationInfos) from
    // data/actor_bootstrap.bin - a stream of full bunch descriptors
    // [u16 chIndex][u8 chType][u8 flags][u16 chSequence][u32 len][payload]. ch0
    // records ride the normal control path; ChIndex>=2 records open their own
    // channel via PacketAssembler::BuildRawBunchPacket. No-op (logged) if absent.
    // NOTE: actor payloads are session-specific (NetGUIDs, the recorded player's
    // state) - this is a best-effort replay; correct per-session actor replication
    // is a later step.
    void SendActorBootstrap(uint32_t clientId);

    // ------------------------------------------------------------------------
    //  CONTROL-CHANNEL INBOUND PATH.
    //
    //  Decodes a raw inbound UDP datagram as a UE3 packet (PacketCodec::Decode),
    //  acknowledges it, feeds its control-channel bunches to the per-connection
    //  reassembler (which orders/dedups them and peels complete messages into the
    //  handshake), and flushes a standalone ack if no response carried it. Wire
    //  format: docs/RS2V_ControlChannel_WireSpec_7258.md.
    // ------------------------------------------------------------------------
    // Returns true if `datagram` was a well-formed UE3 packet and was handled
    // here (so the caller must NOT also run it through the legacy Packet path).
    bool ParseIncomingControl(uint32_t clientId, const std::vector<uint8_t>& datagram);
};
