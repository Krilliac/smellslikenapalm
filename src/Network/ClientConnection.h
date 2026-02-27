// src/Network/ClientConnection.h

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <memory>
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
    bool                ReceiveRaw(std::vector<uint8_t>& outData, PacketMetadata& meta);
    Packet              LastPacket() const;
    std::vector<uint8_t> LastRawPacket() const;

    // Connection state
    void                MarkDisconnected();
    bool                IsDisconnected() const;
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
    std::chrono::steady_clock::time_point m_lastHeartbeat;

    std::string                 m_playerName;
    uint32_t                    m_teamId = 0;

    Packet                      m_lastPacket;
    std::vector<uint8_t>        m_lastRaw;

    // Bandwidth tracking
    uint32_t                    m_sentBytesThisWindow = 0;
    std::chrono::steady_clock::time_point m_windowStart;

    void                        ResetWindowIfNeeded();
};
