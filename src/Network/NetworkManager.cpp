// src/Network/NetworkManager.cpp

#include "Network/NetworkManager.h"
#include "Network/ConnectionManager.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/PacketAnalysis.h"
#include "../../telemetry/TelemetryManager.h"

NetworkManager::NetworkManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[NetworkManager::NetworkManager] Entry: server=%p", (void*)server);
    Logger::Debug("[NetworkManager::NetworkManager] NetworkManager created with GameServer=%p", (void*)server);
    Logger::Trace("[NetworkManager::NetworkManager] Exit");
}

NetworkManager::~NetworkManager() {
    Logger::Trace("[NetworkManager::~NetworkManager] Entry: destructor called");
    Logger::Debug("[NetworkManager::~NetworkManager] Destroying NetworkManager, calling Shutdown()");
    Shutdown();
    Logger::Trace("[NetworkManager::~NetworkManager] Exit");
}

bool NetworkManager::Initialize(uint16_t listenPort) {
    Logger::Trace("[NetworkManager::Initialize] Entry: listenPort=%u", listenPort);
    Logger::Info("NetworkManager: Initializing on port %u", listenPort);

    // Set up connection manager
    Logger::Debug("[NetworkManager::Initialize] Creating ConnectionManager");
    m_connMgr = std::make_unique<ConnectionManager>(m_server);
    Logger::Debug("[NetworkManager::Initialize] Initializing ConnectionManager on port %u", listenPort);
    if (!m_connMgr->Initialize(listenPort)) {
        Logger::Error("[NetworkManager::Initialize] ConnectionManager initialization failed on port %u", listenPort);
        Logger::Trace("[NetworkManager::Initialize] Exit: returning false (ConnMgr init failed)");
        return false;
    }
    Logger::Debug("[NetworkManager::Initialize] ConnectionManager initialized successfully");

    // Bandwidth limit from config
    auto cfgMgr = m_server->GetConfigManager();
    Logger::Debug("[NetworkManager::Initialize] ConfigManager=%p", (void*)cfgMgr.get());
    m_bandwidthLimit = cfgMgr ? (uint32_t)cfgMgr->GetInt("Network.bandwidth_limit", 65536) : 65536;
    Logger::Debug("[NetworkManager::Initialize] Bandwidth limit set to %u bytes/sec", m_bandwidthLimit);
    m_bwManager = std::make_unique<BandwidthManager>(m_bandwidthLimit);
    Logger::Debug("[NetworkManager::Initialize] BandwidthManager created with limit=%u", m_bandwidthLimit);

    // Register our packet callback on ConnectionManager
    Logger::Debug("[NetworkManager::Initialize] Registering packet callback on ConnectionManager");
    m_connMgr->SetPacketCallback([this](uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
        Logger::Debug("[NetworkManager::Initialize::lambda] Packet callback invoked: clientId=%u, tag='%s'",
                      clientId, pkt.GetTag().c_str());
        this->OnPacketReceived(clientId, pkt, meta);
    });

    Logger::Info("NetworkManager initialized");
    Logger::Trace("[NetworkManager::Initialize] Exit: returning true");
    return true;
}

void NetworkManager::Shutdown() {
    Logger::Trace("[NetworkManager::Shutdown] Entry");
    Logger::Info("NetworkManager: Shutting down");
    if (m_connMgr) {
        Logger::Debug("[NetworkManager::Shutdown] Shutting down ConnectionManager");
        m_connMgr->Shutdown();
        m_connMgr.reset();
        Logger::Debug("[NetworkManager::Shutdown] ConnectionManager shutdown and reset");
    } else {
        Logger::Debug("[NetworkManager::Shutdown] ConnectionManager already null");
    }
    Logger::Debug("[NetworkManager::Shutdown] Resetting BandwidthManager");
    m_bwManager.reset();
    Logger::Info("[NetworkManager::Shutdown] Shutdown complete");
    Logger::Trace("[NetworkManager::Shutdown] Exit");
}

void NetworkManager::PollNetwork() {
    Logger::Trace("[NetworkManager::PollNetwork] Entry");
    if (!m_connMgr) {
        Logger::Debug("[NetworkManager::PollNetwork] ConnectionManager is null, returning early");
        Logger::Trace("[NetworkManager::PollNetwork] Exit: no ConnMgr");
        return;
    }
    Logger::Debug("[NetworkManager::PollNetwork] Calling PumpNetwork");
    m_connMgr->PumpNetwork();
    Logger::Debug("[NetworkManager::PollNetwork] Calling RemoveStaleConnections");
    m_connMgr->RemoveStaleConnections();
    if (m_bwManager) {
        Logger::Debug("[NetworkManager::PollNetwork] Calling BandwidthManager::Update");
        m_bwManager->Update();
    }
    Logger::Trace("[NetworkManager::PollNetwork] Exit");
}

void NetworkManager::Flush() {
    Logger::Trace("[NetworkManager::Flush] Entry");
    Logger::Debug("[NetworkManager::Flush] Flush called (no buffered outgoing data to flush)");
    // If outgoing data is buffered, flush here
    Logger::Trace("[NetworkManager::Flush] Exit");
}

bool NetworkManager::SendPacket(uint32_t clientId, const Packet& pkt) {
    Logger::Trace("[NetworkManager::SendPacket] Entry: clientId=%u, tag='%s', payloadSize=%u",
                  clientId, pkt.GetTag().c_str(), pkt.GetPayloadSize());
    auto conn = GetConnection(clientId);
    if (!conn) {
        Logger::Error("[NetworkManager::SendPacket] No connection found for clientId=%u, cannot send tag='%s'",
                      clientId, pkt.GetTag().c_str());
        Logger::Trace("[NetworkManager::SendPacket] Exit: returning false (no connection)");
        return false;
    }
    Logger::Debug("[NetworkManager::SendPacket] Found connection for client %u (%s:%u), sending packet tag='%s'",
                  clientId, conn->GetIP().c_str(), conn->GetPort(), pkt.GetTag().c_str());
    bool result = conn->SendPacket(pkt);
    Logger::Debug("[NetworkManager::SendPacket] SendPacket returned %s for client %u", result ? "true" : "false", clientId);
    Logger::Trace("[NetworkManager::SendPacket] Exit: returning %s", result ? "true" : "false");
    return result;
}

void NetworkManager::BroadcastPacket(const Packet& pkt) {
    Logger::Trace("[NetworkManager::BroadcastPacket] Entry: tag='%s', payloadSize=%u",
                  pkt.GetTag().c_str(), pkt.GetPayloadSize());
    auto connections = GetAllConnections();
    Logger::Debug("[NetworkManager::BroadcastPacket] Broadcasting tag='%s' to %zu clients",
                  pkt.GetTag().c_str(), connections.size());
    for (size_t i = 0; i < connections.size(); ++i) {
        auto& conn = connections[i];
        // HARDENING: GetAllConnections() snapshots shared_ptrs from the client map;
        // skip any null entry rather than deref-crashing mid-broadcast (additive,
        // does not change delivery to valid connections).
        if (!conn) {
            Logger::Warn("[NetworkManager::BroadcastPacket] Null connection at index %zu/%zu, skipping",
                         i + 1, connections.size());
            continue;
        }
        Logger::Trace("[NetworkManager::BroadcastPacket] Sending to client %u [%zu/%zu]",
                      conn->GetClientId(), i + 1, connections.size());
        conn->SendPacket(pkt);
    }
    Logger::Info("[NetworkManager::BroadcastPacket] Broadcast complete: tag='%s' to %zu clients",
                 pkt.GetTag().c_str(), connections.size());
    Logger::Trace("[NetworkManager::BroadcastPacket] Exit");
}

void NetworkManager::BroadcastPacket(const std::string& tag, const std::vector<uint8_t>& data) {
    Logger::Trace("[NetworkManager::BroadcastPacket(tag,data)] Entry: tag='%s', data size=%zu",
                  tag.c_str(), data.size());
    Packet pkt(tag, data);
    Logger::Debug("[NetworkManager::BroadcastPacket(tag,data)] Created packet with tag='%s', payload size=%zu",
                  tag.c_str(), data.size());
    BroadcastPacket(pkt);
    Logger::Trace("[NetworkManager::BroadcastPacket(tag,data)] Exit");
}

uint32_t NetworkManager::GetClientId(const ClientAddress& addr) const {
    Logger::Trace("[NetworkManager::GetClientId] Entry: addr=%s:%u", addr.ip.c_str(), addr.port);
    uint32_t result = m_connMgr ? m_connMgr->FindClientByAddress(addr.ip, addr.port) : UINT32_MAX;
    if (result != UINT32_MAX) {
        Logger::Debug("[NetworkManager::GetClientId] Found clientId=%u for %s:%u", result, addr.ip.c_str(), addr.port);
    } else {
        Logger::Debug("[NetworkManager::GetClientId] No client found for %s:%u (connMgr=%p)",
                      addr.ip.c_str(), addr.port, (void*)m_connMgr.get());
    }
    Logger::Trace("[NetworkManager::GetClientId] Exit: returning %u", result);
    return result;
}

uint32_t NetworkManager::FindClientBySteamID(const std::string& steamId) const {
    Logger::Trace("[NetworkManager::FindClientBySteamID] Entry: steamId='%s'", steamId.c_str());
    uint32_t result = m_connMgr ? m_connMgr->FindClientBySteamID(steamId) : UINT32_MAX;
    if (result != UINT32_MAX) {
        Logger::Debug("[NetworkManager::FindClientBySteamID] Found clientId=%u for steamId='%s'", result, steamId.c_str());
    } else {
        Logger::Debug("[NetworkManager::FindClientBySteamID] No client found for steamId='%s'", steamId.c_str());
    }
    Logger::Trace("[NetworkManager::FindClientBySteamID] Exit: returning %u", result);
    return result;
}

std::shared_ptr<ClientConnection> NetworkManager::GetConnection(uint32_t clientId) const {
    Logger::Trace("[NetworkManager::GetConnection] Entry: clientId=%u", clientId);
    auto result = m_connMgr ? m_connMgr->GetConnection(clientId) : nullptr;
    if (result) {
        Logger::Debug("[NetworkManager::GetConnection] Found connection for client %u (%s:%u)",
                      clientId, result->GetIP().c_str(), result->GetPort());
    } else {
        Logger::Debug("[NetworkManager::GetConnection] No connection found for clientId=%u", clientId);
    }
    Logger::Trace("[NetworkManager::GetConnection] Exit: returning %s", result ? "valid connection" : "nullptr");
    return result;
}

std::vector<std::shared_ptr<ClientConnection>> NetworkManager::GetAllConnections() const {
    Logger::Trace("[NetworkManager::GetAllConnections] Entry");
    auto result = m_connMgr ? m_connMgr->GetAllConnections() : std::vector<std::shared_ptr<ClientConnection>>{};
    Logger::Debug("[NetworkManager::GetAllConnections] Returning %zu connections", result.size());
    Logger::Trace("[NetworkManager::GetAllConnections] Exit: returning %zu connections", result.size());
    return result;
}

uint32_t NetworkManager::GetBandwidthLimit() const {
    Logger::Trace("[NetworkManager::GetBandwidthLimit] Entry/Exit: returning %u", m_bandwidthLimit);
    return m_bandwidthLimit;
}

int NetworkManager::GetPacketsPerSecond() const {
    return m_packetsThisTick;
}

void NetworkManager::SetClientLoggedInCallback(ClientLoggedInCallback cb) {
    Logger::Debug("[NetworkManager::SetClientLoggedInCallback] forwarding to ConnectionManager");
    if (m_connMgr) m_connMgr->SetClientLoggedInCallback(std::move(cb));
    else Logger::Warn("[NetworkManager::SetClientLoggedInCallback] ConnectionManager null, callback dropped");
}

void NetworkManager::SetClientJoinedCallback(ClientJoinedCallback cb) {
    Logger::Debug("[NetworkManager::SetClientJoinedCallback] forwarding to ConnectionManager");
    if (m_connMgr) m_connMgr->SetClientJoinedCallback(std::move(cb));
    else Logger::Warn("[NetworkManager::SetClientJoinedCallback] ConnectionManager null, callback dropped");
}

void NetworkManager::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    m_packetsThisTick++;
    Logger::Trace("[NetworkManager::OnPacketReceived] Entry: clientId=%u, tag='%s', payloadSize=%u",
                  clientId, pkt.GetTag().c_str(), pkt.GetPayloadSize());
    // Dump for analysis
    Logger::Debug("[NetworkManager::OnPacketReceived] Dumping packet for analysis: tag='%s'", pkt.GetTag().c_str());
    DumpPacketForAnalysis(pkt.RawData(), "NetworkManager_OnReceive");

    // Track packet processing in telemetry
    TELEMETRY_INCREMENT_PACKETS_PROCESSED();

    // Enqueue into GameServer's receive queue
    // HARDENING: this fires from the inbound dispatch path; guard against a null
    // GameServer (e.g. during shutdown teardown) so a late packet can't deref-crash
    // the network thread. Drop+log instead (additive; never reached on the valid path).
    if (!m_server) {
        Logger::Warn("[NetworkManager::OnPacketReceived] GameServer is null, dropping packet tag='%s' from client %u",
                     pkt.GetTag().c_str(), clientId);
        Logger::Trace("[NetworkManager::OnPacketReceived] Exit: no GameServer");
        return;
    }
    Logger::Debug("[NetworkManager::OnPacketReceived] Enqueueing packet for client %u into GameServer queue", clientId);
    m_server->EnqueuePacket({ clientId, pkt, meta });
    Logger::Info("[NetworkManager::OnPacketReceived] Packet tag='%s' from client %u enqueued for processing",
                 pkt.GetTag().c_str(), clientId);
    Logger::Trace("[NetworkManager::OnPacketReceived] Exit");
}
