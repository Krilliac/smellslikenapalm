// src/Network/NetworkManager.h

#pragma once

#include <memory>
#include <vector>
#include "Network/BandwidthManager.h"
#include "Network/Packet.h"

class GameServer;
class ConnectionManager;
class ClientConnection;

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

    uint32_t GetClientId(const ClientAddress& addr) const;
    uint32_t FindClientBySteamID(const std::string& steamId) const;
    std::shared_ptr<ClientConnection> GetConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;

    uint32_t GetBandwidthLimit() const;

private:
    GameServer*                                m_server;
    std::unique_ptr<ConnectionManager>         m_connMgr;
    std::unique_ptr<BandwidthManager>          m_bwManager;
    uint32_t                                   m_bandwidthLimit = 65536;

    void OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);
};
