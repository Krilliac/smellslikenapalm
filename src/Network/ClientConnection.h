// src/Network/ClientConnection.h

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <memory>
#include <atomic>
#include "Network/UDPSocket.h"
#include "Network/Packet.h"

class ConnectionManager;

class ClientConnection {
public:
    ClientConnection(uint32_t clientId,
                     const std::string& ip,
                     uint16_t port,
                     std::shared_ptr<UDPSocket> socket,
                     ConnectionManager* manager);
    ~ClientConnection();

    // Identification
    uint32_t            GetClientId() const;
    const std::string&  GetIP() const;
    uint16_t            GetPort() const;
    std::string         GetSteamID() const;

    // Packet I/O
    bool                SendPacket(const Packet& pkt);

    // Send raw bytes (no Packet tag/serialize wrapper). Used for UE3
    // control-channel message payloads produced by ControlChannel::Build*.
    bool                SendRaw(const uint8_t* data, size_t len);
    bool                ReceiveRaw(std::vector<uint8_t>& outData, PacketMetadata& meta);
    Packet              LastPacket() const;
    std::vector<uint8_t> LastRawPacket() const;

    // Connection state
    void                MarkDisconnected();
    bool                IsDisconnected() const;

    // UE3 handshake gating. A real client speaks the UE3 control-channel protocol
    // and CANNOT parse the emulator's internal Packet format. Until the handshake
    // completes (NMT_Join -> Joined), no game-layer Packets may be sent to it -
    // doing so corrupts the client's UE3 packet/sequence state. SendPacket is
    // suppressed while this is false; SendRaw (UE3 control bytes) is always allowed.
    void                SetHandshakeComplete(bool complete) { m_handshakeComplete = complete; }
    bool                IsHandshakeComplete() const { return m_handshakeComplete; }
    void                UpdateLastHeartbeat();
    std::chrono::steady_clock::time_point GetLastHeartbeat() const;

    // Player name management
    void                SetPlayerName(const std::string& name);
    const std::string&  GetPlayerName() const;
    uint32_t            GetTeamId() const;
    void                SetTeamId(uint32_t teamId);

    // Helpers for game layer
    void                SendChatMessage(const std::string& msg);
    void                SendPositionUpdate(const Vector3& pos);
    void                SendOrientationUpdate(const Vector3& dir);
    void                SendHealthUpdate(int hp);
    void                SendTeamUpdate(uint32_t teamId);
    void                SendSpawnPlayer();
    void                SendSessionState(uint32_t aliveCount);
    void                SendInventoryUpdate(const std::vector<struct InventoryItem>& items);

    // Ping measurement
    int                 GetPing() const;
    void                UpdatePing(int pingMs);

    // Rate limiting / QoS
    bool                CanSend(uint32_t byteCount);
    void                OnBytesSent(uint32_t byteCount);

private:
    uint32_t                    m_clientId;
    std::string                 m_ip;
    uint16_t                    m_port;
    std::shared_ptr<UDPSocket>  m_socket;
    ConnectionManager*          m_manager;

    bool                        m_disconnected = false;
    std::atomic<bool>           m_handshakeComplete{false};
    std::chrono::steady_clock::time_point m_lastHeartbeat;

    std::string                 m_playerName;
    uint32_t                    m_teamId = 0;

    Packet                      m_lastPacket;
    std::vector<uint8_t>        m_lastRaw;

    int                         m_pingMs = 0;

    // Bandwidth tracking
    uint32_t                    m_sentBytesThisWindow = 0;
    std::chrono::steady_clock::time_point m_windowStart;

    void                        ResetWindowIfNeeded();
};
