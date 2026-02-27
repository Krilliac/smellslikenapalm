// src/Network/NetworkManager.cpp

#include "Network/NetworkManager.h"
#include "Network/ConnectionManager.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/PacketAnalysis.h"

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

    // Bandwidth limit from config
    auto cfgMgr = m_server->GetConfigManager();
    m_bandwidthLimit = cfgMgr ? (uint32_t)cfgMgr->GetInt("Network.bandwidth_limit", 65536) : 65536;
    m_bwManager = std::make_unique<BandwidthManager>(m_bandwidthLimit);

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

void NetworkManager::PollNetwork() {
    if (!m_connMgr) return;
    m_connMgr->PumpNetwork();
    m_connMgr->RemoveStaleConnections();
    if (m_bwManager) m_bwManager->Update();
}

void NetworkManager::Flush() {
    // If outgoing data is buffered, flush here
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
    return m_connMgr ? m_connMgr->FindClientByAddress(addr.ip, addr.port) : UINT32_MAX;
}

uint32_t NetworkManager::FindClientBySteamID(const std::string& steamId) const {
    return m_connMgr ? m_connMgr->FindClientBySteamID(steamId) : UINT32_MAX;
}

std::shared_ptr<ClientConnection> NetworkManager::GetConnection(uint32_t clientId) const {
    return m_connMgr ? m_connMgr->GetConnection(clientId) : nullptr;
}

std::vector<std::shared_ptr<ClientConnection>> NetworkManager::GetAllConnections() const {
    return m_connMgr ? m_connMgr->GetAllConnections() : std::vector<std::shared_ptr<ClientConnection>>{};
}

uint32_t NetworkManager::GetBandwidthLimit() const {
    return m_bandwidthLimit;
}

void NetworkManager::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    // Dump for analysis
    DumpPacketForAnalysis(pkt.RawData(), "NetworkManager_OnReceive");

    // Enqueue into GameServer's receive queue
    m_server->EnqueuePacket({ clientId, pkt, meta });
}
