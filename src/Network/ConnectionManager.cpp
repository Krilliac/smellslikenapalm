// src/Network/ConnectionManager.cpp – Implementation for ConnectionManager

#include "Network/ConnectionManager.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include <chrono>

ConnectionManager::ConnectionManager(GameServer* server)
    : m_server(server)
{}

ConnectionManager::~ConnectionManager() {
    Shutdown();
}

bool ConnectionManager::Initialize(uint16_t listenPort) {
    Logger::Info("ConnectionManager: Initializing on port %u", listenPort);
    m_socket = std::make_shared<UDPSocket>();
    if (!m_socket->Bind(listenPort)) {
        Logger::Error("ConnectionManager: Failed to bind UDP socket on port %u", listenPort);
        return false;
    }
    m_bwManager = std::make_unique<BandwidthManager>(m_server->GetConfigManager()->GetInt("Network.MaxPacketSize", 1200));
    Logger::Info("ConnectionManager: Initialized successfully");
    return true;
}

void ConnectionManager::Shutdown() {
    Logger::Info("ConnectionManager: Shutting down");
    if (m_socket) {
        m_socket->Close();
        m_socket.reset();
    }
    m_clients.clear();
    m_bwManager.reset();
}

void ConnectionManager::PumpNetwork() {
    std::vector<uint8_t> buffer(1500);
    std::string srcIp;
    uint16_t srcPort;
    int len = m_socket->ReceiveFrom(srcIp, srcPort, buffer.data(), (int)buffer.size());
    if (len <= 0) return;

    buffer.resize(len);
    ClientAddress addr{srcIp, srcPort};
    // Bandwidth check
    if (!m_bwManager->CanReceivePacket(addr, len)) return;
    m_bwManager->OnPacketSent(addr, len);

    HandleIncomingPacket(buffer, addr);
}

void ConnectionManager::HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr) {
    uint32_t clientId = CreateOrGetClient(addr.ip, addr.port);
    auto conn = m_clients[addr];
    PacketMetadata meta;
    Packet pkt = Packet::FromBuffer(data, meta);
    conn->UpdateLastHeartbeat();

    // Dispatch to game server
    m_server->OnPacketReceived(clientId, pkt, meta);
}

bool ConnectionManager::SendToClient(uint32_t clientId, const Packet& pkt) {
    auto conn = GetConnection(clientId);
    if (!conn) return false;
    return conn->SendPacket(pkt);
}

void ConnectionManager::Broadcast(const Packet& pkt) {
    for (auto& conn : GetAllConnections()) {
        conn->SendPacket(pkt);
    }
}

std::shared_ptr<ClientConnection> ConnectionManager::GetConnection(uint32_t clientId) const {
    for (auto& kv : m_clients) {
        if (kv.second->GetClientId() == clientId) {
            return kv.second;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<ClientConnection>> ConnectionManager::GetAllConnections() const {
    std::vector<std::shared_ptr<ClientConnection>> list;
    for (auto& kv : m_clients) {
        list.push_back(kv.second);
    }
    return list;
}

uint32_t ConnectionManager::FindClientByAddress(const std::string& ip, uint16_t port) const {
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    return it != m_clients.end() ? it->second->GetClientId() : UINT32_MAX;
}

uint32_t ConnectionManager::CreateOrGetClient(const std::string& ip, uint16_t port) {
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    if (it != m_clients.end()) {
        return it->second->GetClientId();
    }
    if (m_clients.size() >= m_maxClients) {
        Logger::Warn("ConnectionManager: Max clients reached, rejecting new client from %s:%u", ip.c_str(), port);
        return UINT32_MAX;
    }
    uint32_t clientId = m_nextClientId++;
    auto conn = std::make_shared<ClientConnection>(clientId, ip, port, m_socket, this);
    m_clients[addr] = conn;
    Logger::Info("ConnectionManager: New client %u from %s:%u", clientId, ip.c_str(), port);
    return clientId;
}

void ConnectionManager::RemoveStaleConnections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<ClientAddress> toRemove;
    for (auto& kv : m_clients) {
        auto conn = kv.second;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->GetLastHeartbeat()).count();
        if (elapsed > m_server->GetConfigManager()->GetInt("Network.TimeoutSeconds", 30)) {
            Logger::Info("ConnectionManager: Timing out client %u", conn->GetClientId());
            conn->MarkDisconnected();
            toRemove.push_back(kv.first);
        }
    }
    for (auto& addr : toRemove) {
        m_clients.erase(addr);
    }
}

void ConnectionManager::UpdateBandwidthWindows() {
    m_bwManager->Update();
}

void ConnectionManager::SetMaxClients(size_t maxClients) {
    m_maxClients = maxClients;
}

size_t ConnectionManager::GetMaxClients() const {
    return m_maxClients;
}