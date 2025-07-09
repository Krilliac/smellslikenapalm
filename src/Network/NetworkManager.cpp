// src/Network/NetworkManager.cpp

#include "Network/NetworkManager.h"
#include "Game/GameServer.h"
#include "Utils/Logger.h"
#include "Network/PacketHandler.h"  // if using PacketHandler for dispatch

NetworkManager::NetworkManager(GameServer* server)
    : m_server(server)
{}

NetworkManager::~NetworkManager() {
    Shutdown();
}

bool NetworkManager::Initialize(uint16_t listenPort) {
    Logger::Info("NetworkManager: Initializing on port %u", listenPort);

    // Set up connection manager
    m_connMgr = std::make_unique<ConnectionManager>(m_server);
    if (!m_connMgr->Initialize(listenPort)) return false;

    // Bandwidth limit
    uint32_t bwLimit = m_server->GetConfigManager()->GetInt("Network.MaxPacketSize", 1200);
    m_bwManager = std::make_unique<BandwidthManager>(bwLimit);

    // Register our packet callback on ConnectionManager
    m_connMgr->SetPacketCallback([this](uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
        this->OnPacketReceived(clientId, pkt, meta);
    });

    Logger::Info("NetworkManager initialized");
    return true;
}

void NetworkManager::Shutdown() {
    Logger::Info("NetworkManager: Shutting down");
    if (m_connMgr) {
        m_connMgr->Shutdown();
        m_connMgr.reset();
    }
    m_bwManager.reset();
}

std::vector<ReceivedPacket> NetworkManager::ReceivePackets() {
    // Poll network I/O
    m_connMgr->PumpNetwork();
    m_connMgr->RemoveStaleConnections();
    m_connMgr->UpdateBandwidthWindows();

    // Return all packets queued via EnqueuePacket
    return m_server->FetchPendingPackets();
}

void NetworkManager::Flush() {
    // If you buffer outgoing, flush here
}

bool NetworkManager::SendPacket(uint32_t clientId, const Packet& pkt) {
    auto conn = GetConnection(clientId);
    if (!conn) return false;
    return conn->SendPacket(pkt);
}

void NetworkManager::BroadcastPacket(const Packet& pkt) {
    for (auto& conn : GetAllConnections()) {
        conn->SendPacket(pkt);
    }
}

void NetworkManager::BroadcastPacket(const std::string& tag, const std::vector<uint8_t>& data) {
    Packet pkt(tag, data);
    BroadcastPacket(pkt);
}

uint32_t NetworkManager::GetClientId(const ClientAddress& addr) const {
    return m_connMgr->FindClientByAddress(addr.ip, addr.port);
}

uint32_t NetworkManager::FindClientBySteamID(const std::string& steamId) const {
    // Use ConnectionManager’s mapping via SteamID if implemented
    return m_connMgr->FindClientBySteamID(steamId);
}

std::shared_ptr<ClientConnection> NetworkManager::GetConnection(uint32_t clientId) const {
    return m_connMgr->GetConnection(clientId);
}

std::vector<std::shared_ptr<ClientConnection>> NetworkManager::GetAllConnections() const {
    return m_connMgr->GetAllConnections();
}

uint32_t NetworkManager::GetBandwidthLimit() const {
    return m_bwManager ? m_bwManager->m_maxBytesPerSec : 0;
}

void NetworkManager::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    // Dump for analysis
    DumpPacketForAnalysis(pkt.RawData(), "NetworkManager_OnReceive");

    // Enqueue into GameServer’s receive queue
    m_server->EnqueuePacket({ clientId, pkt, meta });
}