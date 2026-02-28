// src/Network/ConnectionManager.cpp

#include "Network/ConnectionManager.h"
#include "Game/GameServer.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "../../telemetry/TelemetryManager.h"
#include <chrono>

ConnectionManager::ConnectionManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ConnectionManager::ConnectionManager] Entry: server=%p", (void*)server);
    Logger::Debug("[ConnectionManager::ConnectionManager] ConnectionManager created with GameServer=%p", (void*)server);
    Logger::Trace("[ConnectionManager::ConnectionManager] Exit");
}

ConnectionManager::~ConnectionManager() {
    Logger::Trace("[ConnectionManager::~ConnectionManager] Entry: destructor called");
    Logger::Debug("[ConnectionManager::~ConnectionManager] Destroying ConnectionManager, %zu clients connected", m_clients.size());
    Shutdown();
    Logger::Trace("[ConnectionManager::~ConnectionManager] Exit");
}

bool ConnectionManager::Initialize(uint16_t listenPort) {
    Logger::Trace("[ConnectionManager::Initialize] Entry: listenPort=%u", listenPort);
    Logger::Info("ConnectionManager: Initializing on port %u", listenPort);
    m_socket = std::make_shared<UDPSocket>();
    Logger::Debug("[ConnectionManager::Initialize] Created UDPSocket, attempting bind on port %u", listenPort);
    if (!m_socket->Bind(listenPort)) {
        Logger::Error("ConnectionManager: Failed to bind UDP socket on port %u", listenPort);
        Logger::Trace("[ConnectionManager::Initialize] Exit: returning false (bind failed)");
        return false;
    }
    Logger::Debug("[ConnectionManager::Initialize] UDP socket bound successfully on port %u", listenPort);

    auto cfgMgr = m_server->GetConfigManager();
    Logger::Debug("[ConnectionManager::Initialize] ConfigManager=%p", (void*)cfgMgr.get());
    m_bandwidthLimit = cfgMgr ? (uint32_t)cfgMgr->GetInt("Network.bandwidth_limit", 65536) : 65536;
    Logger::Debug("[ConnectionManager::Initialize] Bandwidth limit set to %u bytes/sec", m_bandwidthLimit);
    m_bwManager = std::make_unique<BandwidthManager>(m_bandwidthLimit);
    Logger::Debug("[ConnectionManager::Initialize] BandwidthManager created with limit=%u", m_bandwidthLimit);

    Logger::Info("ConnectionManager: Initialized successfully");
    Logger::Trace("[ConnectionManager::Initialize] Exit: returning true");
    return true;
}

void ConnectionManager::Shutdown() {
    Logger::Trace("[ConnectionManager::Shutdown] Entry");
    Logger::Info("ConnectionManager: Shutting down");
    if (m_socket) {
        Logger::Debug("[ConnectionManager::Shutdown] Closing socket and resetting");
        m_socket->Close();
        m_socket.reset();
    } else {
        Logger::Debug("[ConnectionManager::Shutdown] Socket already null");
    }
    Logger::Debug("[ConnectionManager::Shutdown] Clearing %zu client connections", m_clients.size());
    m_clients.clear();
    Logger::Debug("[ConnectionManager::Shutdown] Resetting BandwidthManager");
    m_bwManager.reset();
    Logger::Info("[ConnectionManager::Shutdown] Shutdown complete");
    Logger::Trace("[ConnectionManager::Shutdown] Exit");
}

void ConnectionManager::SetPacketCallback(PacketCallback cb) {
    Logger::Trace("[ConnectionManager::SetPacketCallback] Entry: cb=%s", cb ? "non-null" : "null");
    m_packetCallback = std::move(cb);
    Logger::Debug("[ConnectionManager::SetPacketCallback] Packet callback set");
    Logger::Trace("[ConnectionManager::SetPacketCallback] Exit");
}

void ConnectionManager::PumpNetwork() {
    Logger::Trace("[ConnectionManager::PumpNetwork] Entry");
    if (!m_socket || !m_socket->IsOpen()) {
        Logger::Debug("[ConnectionManager::PumpNetwork] Socket not available or not open, returning early");
        Logger::Trace("[ConnectionManager::PumpNetwork] Exit: socket not ready");
        return;
    }

    // Drain all available packets from the socket
    Logger::Debug("[ConnectionManager::PumpNetwork] Draining packets (max 256 per pump)");
    int packetCount = 0;
    for (int i = 0; i < 256; ++i) {
        std::vector<uint8_t> buffer(1500);
        std::string srcIp;
        uint16_t srcPort = 0;
        int len = m_socket->ReceiveFrom(srcIp, srcPort, buffer.data(), (int)buffer.size());
        if (len <= 0) {
            Logger::Trace("[ConnectionManager::PumpNetwork] ReceiveFrom returned %d at iteration %d, stopping drain", len, i);
            break;
        }

        buffer.resize(len);
        ClientAddress addr{srcIp, srcPort};
        Logger::Debug("[ConnectionManager::PumpNetwork] Received %d bytes from %s:%u (iteration %d)",
                      len, srcIp.c_str(), srcPort, i);
        // Bandwidth check
        if (m_bwManager && !m_bwManager->CanReceivePacket(addr, (uint32_t)len)) {
            Logger::Warn("[ConnectionManager::PumpNetwork] Bandwidth limit exceeded for %s:%u, dropping %d byte packet",
                         srcIp.c_str(), srcPort, len);
            TELEMETRY_INCREMENT_PACKETS_DROPPED();
            continue;
        }

        Logger::Debug("[ConnectionManager::PumpNetwork] Bandwidth check passed, handling packet from %s:%u",
                      srcIp.c_str(), srcPort);
        HandleIncomingPacket(buffer, addr);
        packetCount++;
    }
    Logger::Debug("[ConnectionManager::PumpNetwork] Drained %d packets this pump cycle", packetCount);
    Logger::Trace("[ConnectionManager::PumpNetwork] Exit");
}

void ConnectionManager::HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr) {
    Logger::Trace("[ConnectionManager::HandleIncomingPacket] Entry: addr=%s:%u, data size=%zu",
                  addr.ip.c_str(), addr.port, data.size());
    uint32_t clientId = CreateOrGetClient(addr.ip, addr.port);
    if (clientId == UINT32_MAX) {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] CreateOrGetClient returned UINT32_MAX for %s:%u, dropping packet",
                     addr.ip.c_str(), addr.port);
        Logger::Trace("[ConnectionManager::HandleIncomingPacket] Exit: client creation failed");
        return;
    }
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] clientId=%u for %s:%u", clientId, addr.ip.c_str(), addr.port);

    // Feed raw UDP data to protocol decoder for UE3 bunch analysis
    GetProtocolDecoder().OnRawUDPReceived(clientId, data.data(), data.size());
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Fed %zu raw bytes to protocol decoder for client %u",
                  data.size(), clientId);

    auto conn = m_clients[addr];
    PacketMetadata meta;
    meta.clientId = clientId;
    Packet pkt = Packet::FromBuffer(data, meta);
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Parsed packet: tag='%s', clientId=%u, payloadSize=%u",
                  pkt.GetTag().c_str(), clientId, pkt.GetPayloadSize());
    conn->UpdateLastHeartbeat();
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Updated heartbeat for client %u", clientId);

    // Feed parsed packet to protocol decoder for structure analysis
    GetProtocolDecoder().OnPacketReceived(clientId, pkt.RawData(), pkt.GetTag());
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Fed parsed packet (tag='%s') to protocol decoder", pkt.GetTag().c_str());

    // Dispatch via callback (to NetworkManager -> GameServer) or directly
    if (m_packetCallback) {
        Logger::Debug("[ConnectionManager::HandleIncomingPacket] Dispatching via packet callback for client %u, tag='%s'",
                      clientId, pkt.GetTag().c_str());
        m_packetCallback(clientId, pkt, meta);
    } else if (m_server) {
        Logger::Debug("[ConnectionManager::HandleIncomingPacket] Dispatching directly to GameServer for client %u, tag='%s'",
                      clientId, pkt.GetTag().c_str());
        m_server->OnPacketReceived(clientId, pkt, meta);
    } else {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] No callback and no server set, packet tag='%s' from client %u dropped",
                     pkt.GetTag().c_str(), clientId);
    }
    Logger::Trace("[ConnectionManager::HandleIncomingPacket] Exit");
}

bool ConnectionManager::SendToClient(uint32_t clientId, const Packet& pkt) {
    Logger::Trace("[ConnectionManager::SendToClient] Entry: clientId=%u, tag='%s'", clientId, pkt.GetTag().c_str());
    auto conn = GetConnection(clientId);
    if (!conn) {
        Logger::Error("[ConnectionManager::SendToClient] No connection found for clientId=%u", clientId);
        Logger::Trace("[ConnectionManager::SendToClient] Exit: returning false (no connection)");
        return false;
    }
    Logger::Debug("[ConnectionManager::SendToClient] Found connection for client %u (%s:%u), sending packet tag='%s'",
                  clientId, conn->GetIP().c_str(), conn->GetPort(), pkt.GetTag().c_str());
    bool result = conn->SendPacket(pkt);
    Logger::Debug("[ConnectionManager::SendToClient] SendPacket returned %s for client %u", result ? "true" : "false", clientId);
    Logger::Trace("[ConnectionManager::SendToClient] Exit: returning %s", result ? "true" : "false");
    return result;
}

void ConnectionManager::Broadcast(const Packet& pkt) {
    Logger::Trace("[ConnectionManager::Broadcast] Entry: tag='%s'", pkt.GetTag().c_str());
    auto connections = GetAllConnections();
    Logger::Debug("[ConnectionManager::Broadcast] Broadcasting packet tag='%s' to %zu clients",
                  pkt.GetTag().c_str(), connections.size());
    for (size_t i = 0; i < connections.size(); ++i) {
        auto& conn = connections[i];
        Logger::Trace("[ConnectionManager::Broadcast] Sending to client %u (%s:%u) [%zu/%zu]",
                      conn->GetClientId(), conn->GetIP().c_str(), conn->GetPort(), i + 1, connections.size());
        conn->SendPacket(pkt);
    }
    Logger::Info("[ConnectionManager::Broadcast] Broadcast complete: tag='%s' sent to %zu clients",
                 pkt.GetTag().c_str(), connections.size());
    Logger::Trace("[ConnectionManager::Broadcast] Exit");
}

std::shared_ptr<ClientConnection> ConnectionManager::GetConnection(uint32_t clientId) const {
    Logger::Trace("[ConnectionManager::GetConnection] Entry: clientId=%u", clientId);
    for (auto& kv : m_clients) {
        if (kv.second->GetClientId() == clientId) {
            Logger::Debug("[ConnectionManager::GetConnection] Found connection for client %u at %s:%u",
                          clientId, kv.second->GetIP().c_str(), kv.second->GetPort());
            Logger::Trace("[ConnectionManager::GetConnection] Exit: returning valid connection");
            return kv.second;
        }
    }
    Logger::Debug("[ConnectionManager::GetConnection] No connection found for clientId=%u (searched %zu entries)",
                  clientId, m_clients.size());
    Logger::Trace("[ConnectionManager::GetConnection] Exit: returning nullptr");
    return nullptr;
}

std::vector<std::shared_ptr<ClientConnection>> ConnectionManager::GetAllConnections() const {
    Logger::Trace("[ConnectionManager::GetAllConnections] Entry");
    std::vector<std::shared_ptr<ClientConnection>> list;
    list.reserve(m_clients.size());
    for (auto& kv : m_clients) {
        list.push_back(kv.second);
    }
    Logger::Debug("[ConnectionManager::GetAllConnections] Collected %zu connections", list.size());
    Logger::Trace("[ConnectionManager::GetAllConnections] Exit: returning %zu connections", list.size());
    return list;
}

uint32_t ConnectionManager::FindClientByAddress(const std::string& ip, uint16_t port) const {
    Logger::Trace("[ConnectionManager::FindClientByAddress] Entry: ip='%s', port=%u", ip.c_str(), port);
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    uint32_t result = it != m_clients.end() ? it->second->GetClientId() : UINT32_MAX;
    if (result != UINT32_MAX) {
        Logger::Debug("[ConnectionManager::FindClientByAddress] Found client %u at %s:%u", result, ip.c_str(), port);
    } else {
        Logger::Debug("[ConnectionManager::FindClientByAddress] No client found at %s:%u", ip.c_str(), port);
    }
    Logger::Trace("[ConnectionManager::FindClientByAddress] Exit: returning %u", result);
    return result;
}

uint32_t ConnectionManager::FindClientBySteamID(const std::string& steamId) const {
    Logger::Trace("[ConnectionManager::FindClientBySteamID] Entry: steamId='%s'", steamId.c_str());
    for (auto& kv : m_clients) {
        if (kv.second->GetSteamID() == steamId) {
            uint32_t clientId = kv.second->GetClientId();
            Logger::Debug("[ConnectionManager::FindClientBySteamID] Found client %u with steamId='%s'", clientId, steamId.c_str());
            Logger::Trace("[ConnectionManager::FindClientBySteamID] Exit: returning %u", clientId);
            return clientId;
        }
    }
    Logger::Debug("[ConnectionManager::FindClientBySteamID] No client found with steamId='%s' (searched %zu entries)",
                  steamId.c_str(), m_clients.size());
    Logger::Trace("[ConnectionManager::FindClientBySteamID] Exit: returning UINT32_MAX");
    return UINT32_MAX;
}

uint32_t ConnectionManager::CreateOrGetClient(const std::string& ip, uint16_t port) {
    Logger::Trace("[ConnectionManager::CreateOrGetClient] Entry: ip='%s', port=%u", ip.c_str(), port);
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    if (it != m_clients.end()) {
        uint32_t existingId = it->second->GetClientId();
        Logger::Debug("[ConnectionManager::CreateOrGetClient] Existing client found: clientId=%u for %s:%u",
                      existingId, ip.c_str(), port);
        Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning existing clientId=%u", existingId);
        return existingId;
    }
    Logger::Debug("[ConnectionManager::CreateOrGetClient] No existing client for %s:%u, checking capacity (%zu/%zu)",
                  ip.c_str(), port, m_clients.size(), m_maxClients);
    if (m_clients.size() >= m_maxClients) {
        Logger::Warn("ConnectionManager: Max clients reached, rejecting new client from %s:%u", ip.c_str(), port);
        Logger::Debug("[ConnectionManager::CreateOrGetClient] Max clients %zu reached, cannot create new client",
                      m_maxClients);
        Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning UINT32_MAX (max clients)");
        return UINT32_MAX;
    }
    uint32_t clientId = m_nextClientId++;
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Assigned new clientId=%u (nextClientId now=%u)",
                  clientId, m_nextClientId);
    auto conn = std::make_shared<ClientConnection>(clientId, ip, port, m_socket, this);
    m_clients[addr] = conn;
    Logger::Info("ConnectionManager: New client %u from %s:%u", clientId, ip.c_str(), port);
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Total clients now: %zu", m_clients.size());

    // Update telemetry connection metrics
    TELEMETRY_UPDATE_PLAYER_COUNTS(m_clients.size(), m_clients.size());

    // Notify protocol decoder of new client connection
    GetProtocolDecoder().OnClientConnected(clientId, ip);
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Protocol decoder notified of new client %u", clientId);

    Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning new clientId=%u", clientId);
    return clientId;
}

void ConnectionManager::RemoveStaleConnections() {
    Logger::Trace("[ConnectionManager::RemoveStaleConnections] Entry: %zu clients", m_clients.size());
    auto now = std::chrono::steady_clock::now();
    auto cfgMgr = m_server->GetConfigManager();
    int timeoutSecs = cfgMgr ? cfgMgr->GetInt("Network.timeout_seconds", 30) : 30;
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] Timeout threshold: %d seconds", timeoutSecs);

    std::vector<ClientAddress> toRemove;
    for (auto& kv : m_clients) {
        auto conn = kv.second;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->GetLastHeartbeat()).count();
        Logger::Trace("[ConnectionManager::RemoveStaleConnections] Client %u (%s:%u): elapsed=%lld s since last heartbeat",
                      conn->GetClientId(), conn->GetIP().c_str(), conn->GetPort(), (long long)elapsed);
        if (elapsed > timeoutSecs) {
            Logger::Info("ConnectionManager: Timing out client %u", conn->GetClientId());
            Logger::Debug("[ConnectionManager::RemoveStaleConnections] Client %u exceeded timeout (%lld > %d), marking for removal",
                          conn->GetClientId(), (long long)elapsed, timeoutSecs);
            GetProtocolDecoder().OnClientDisconnected(conn->GetClientId());
            conn->MarkDisconnected();
            toRemove.push_back(kv.first);

            // Track disconnection in telemetry
            TELEMETRY_UPDATE_PLAYER_COUNTS(m_clients.size() - toRemove.size(), m_clients.size() - toRemove.size());
        }
    }
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] Removing %zu stale connections", toRemove.size());
    for (auto& addr : toRemove) {
        Logger::Debug("[ConnectionManager::RemoveStaleConnections] Erasing client at %s:%u", addr.ip.c_str(), addr.port);
        m_clients.erase(addr);
    }
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] After cleanup: %zu clients remaining", m_clients.size());
    Logger::Trace("[ConnectionManager::RemoveStaleConnections] Exit");
}

void ConnectionManager::UpdateBandwidthWindows() {
    Logger::Trace("[ConnectionManager::UpdateBandwidthWindows] Entry");
    if (m_bwManager) {
        Logger::Debug("[ConnectionManager::UpdateBandwidthWindows] Calling BandwidthManager::Update()");
        m_bwManager->Update();
    } else {
        Logger::Debug("[ConnectionManager::UpdateBandwidthWindows] No BandwidthManager set, skipping");
    }
    Logger::Trace("[ConnectionManager::UpdateBandwidthWindows] Exit");
}

uint32_t ConnectionManager::GetBandwidthLimit() const {
    Logger::Trace("[ConnectionManager::GetBandwidthLimit] Entry/Exit: returning %u", m_bandwidthLimit);
    return m_bandwidthLimit;
}

void ConnectionManager::SetMaxClients(size_t maxClients) {
    Logger::Trace("[ConnectionManager::SetMaxClients] Entry: maxClients=%zu", maxClients);
    size_t previous = m_maxClients;
    m_maxClients = maxClients;
    Logger::Debug("[ConnectionManager::SetMaxClients] Max clients changed: %zu -> %zu", previous, m_maxClients);
    Logger::Info("[ConnectionManager::SetMaxClients] Max clients set to %zu", maxClients);
    Logger::Trace("[ConnectionManager::SetMaxClients] Exit");
}

size_t ConnectionManager::GetMaxClients() const {
    Logger::Trace("[ConnectionManager::GetMaxClients] Entry/Exit: returning %zu", m_maxClients);
    return m_maxClients;
}
