// src/Network/ConnectionManager.h – Header for ConnectionManager

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include "Network/ClientConnection.h"
#include "Network/UDPSocket.h"
#include "Game/GameServer.h"

class ConnectionManager {
public:
    explicit ConnectionManager(GameServer* server);
    ~ConnectionManager();

    // Initialize networking subsystems
    bool Initialize(uint16_t listenPort);
    void Shutdown();

    // Main loop: receive raw data and dispatch to handlers
    void PumpNetwork();

    // Send utilities
    bool SendToClient(uint32_t clientId, const Packet& pkt);
    void Broadcast(const Packet& pkt);

    // Client management
    std::shared_ptr<ClientConnection> GetConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;
    uint32_t FindClientByAddress(const std::string& ip, uint16_t port) const;
    uint32_t CreateOrGetClient(const std::string& ip, uint16_t port);

    // Periodic housekeeping
    void RemoveStaleConnections();
    void UpdateBandwidthWindows();

    // Configuration
    void SetMaxClients(size_t maxClients);
    size_t GetMaxClients() const;

private:
    GameServer* m_server;
    std::shared_ptr<UDPSocket> m_socket;

    // Client lookup by address
    std::unordered_map<ClientAddress, std::shared_ptr<ClientConnection>> m_clients;
    uint32_t m_nextClientId{1};
    size_t m_maxClients{256};

    // Bandwidth manager
    std::unique_ptr<BandwidthManager> m_bwManager;

    // Helpers
    void HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr);
};