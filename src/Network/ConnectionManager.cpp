// src/Network/ConnectionManager.cpp

#include "Network/ConnectionManager.h"
#include "Game/GameServer.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
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

    auto cfgMgr = m_server->GetConfigManager();
    m_bandwidthLimit = cfgMgr ? (uint32_t)cfgMgr->GetInt("Network.bandwidth_limit", 65536) : 65536;
    m_bwManager = std::make_unique<BandwidthManager>(m_bandwidthLimit);

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

void ConnectionManager::SetPacketCallback(PacketCallback cb) {
    m_packetCallback = std::move(cb);
}

void ConnectionManager::PumpNetwork() {
    if (!m_socket || !m_socket->IsOpen()) return;

    // Drain all available packets from the socket
    for (int i = 0; i < 256; ++i) {
        std::vector<uint8_t> buffer(1500);
        std::string srcIp;
        uint16_t srcPort = 0;
        int len = m_socket->ReceiveFrom(srcIp, srcPort, buffer.data(), (int)buffer.size());
        if (len <= 0) break;

        buffer.resize(len);
        ClientAddress addr{srcIp, srcPort};
        // Bandwidth check
        if (m_bwManager && !m_bwManager->CanReceivePacket(addr, (uint32_t)len)) continue;

        HandleIncomingPacket(buffer, addr);
    }
}

void ConnectionManager::HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr) {
    uint32_t clientId = CreateOrGetClient(addr.ip, addr.port);
    if (clientId == UINT32_MAX) return;

    // Feed raw UDP data to protocol decoder for UE3 bunch analysis
    GetProtocolDecoder().OnRawUDPReceived(clientId, data.data(), data.size());

    auto conn = m_clients[addr];
    PacketMetadata meta;
    meta.clientId = clientId;
    Packet pkt = Packet::FromBuffer(data, meta);
    conn->UpdateLastHeartbeat();

    // Feed parsed packet to protocol decoder for structure analysis
    GetProtocolDecoder().OnPacketReceived(clientId, pkt.RawData(), pkt.GetTag());

    // Dispatch via callback (to NetworkManager -> GameServer) or directly
    if (m_packetCallback) {
        m_packetCallback(clientId, pkt, meta);
    } else if (m_server) {
        m_server->OnPacketReceived(clientId, pkt, meta);
    }
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
    list.reserve(m_clients.size());
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

uint32_t ConnectionManager::FindClientBySteamID(const std::string& steamId) const {
    for (auto& kv : m_clients) {
        if (kv.second->GetSteamID() == steamId) {
            return kv.second->GetClientId();
        }
    }
    return UINT32_MAX;
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

    // Notify protocol decoder of new client connection
    GetProtocolDecoder().OnClientConnected(clientId, ip);

    return clientId;
}

void ConnectionManager::RemoveStaleConnections() {
    auto now = std::chrono::steady_clock::now();
    auto cfgMgr = m_server->GetConfigManager();
    int timeoutSecs = cfgMgr ? cfgMgr->GetInt("Network.timeout_seconds", 30) : 30;

    std::vector<ClientAddress> toRemove;
    for (auto& kv : m_clients) {
        auto conn = kv.second;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->GetLastHeartbeat()).count();
        if (elapsed > timeoutSecs) {
            Logger::Info("ConnectionManager: Timing out client %u", conn->GetClientId());
            GetProtocolDecoder().OnClientDisconnected(conn->GetClientId());
            conn->MarkDisconnected();
            toRemove.push_back(kv.first);
        }
    }
    for (auto& addr : toRemove) {
        m_clients.erase(addr);
    }
}

void ConnectionManager::UpdateBandwidthWindows() {
    if (m_bwManager) m_bwManager->Update();
}

uint32_t ConnectionManager::GetBandwidthLimit() const {
    return m_bandwidthLimit;
}

void ConnectionManager::SetMaxClients(size_t maxClients) {
    m_maxClients = maxClients;
}

size_t ConnectionManager::GetMaxClients() const {
    return m_maxClients;
}
