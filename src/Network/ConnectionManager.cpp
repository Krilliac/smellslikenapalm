// src/Network/ConnectionManager.cpp

#include "Network/ConnectionManager.h"
#include "Game/GameServer.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "../../telemetry/TelemetryManager.h"
#include <chrono>
#include <fstream>
#include <mutex>

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

    auto conn = m_clients[addr];
    conn->UpdateLastHeartbeat();
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Updated heartbeat for client %u", clientId);

    // ---- UE3 control-channel path (EXCLUSIVE) ----
    // A well-formed UE3 packet is handled ONLY here. Running it through the legacy
    // Packet pipeline below would mis-parse the UE3 bytes as a tagged Packet
    // (Packet::FromBuffer) and enqueue garbage into the game queue (observed as a
    // flood of "Rejecting malformed buffer" warns + bogus callback dispatches).
    GetProtocolDecoder().OnRawUDPReceived(clientId, data.data(), data.size());
    if (ParseIncomingControl(clientId, data)) {
        Logger::Trace("[ConnectionManager::HandleIncomingPacket] client %u: handled as UE3 control packet", clientId);
        return;
    }

    // ---- legacy / non-UE3 path (internal Packet format: in-process tests, tools) ----
    PacketMetadata meta;
    meta.clientId = clientId;
    Packet pkt = Packet::FromBuffer(data, meta);
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Parsed packet: tag='%s', clientId=%u, payloadSize=%u",
                  pkt.GetTag().c_str(), clientId, pkt.GetPayloadSize());

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
            m_handshakes.erase(conn->GetClientId());
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

// ===========================================================================
//  Game-facing handshake observer interface
// ===========================================================================

void ConnectionManager::SetClientLoggedInCallback(ClientLoggedInCallback cb) {
    Logger::Debug("[ConnectionManager::SetClientLoggedInCallback] %s", cb ? "set" : "cleared");
    m_clientLoggedInCb = std::move(cb);
}

void ConnectionManager::SetClientJoinedCallback(ClientJoinedCallback cb) {
    Logger::Debug("[ConnectionManager::SetClientJoinedCallback] %s", cb ? "set" : "cleared");
    m_clientJoinedCb = std::move(cb);
}

void ConnectionManager::FireClientLoggedIn(const ClientLoggedInEvent& ev) {
    Logger::Info("[ConnectionManager::FireClientLoggedIn] client %u logged in (steamId=%llu, name='%s')",
                 ev.clientId, (unsigned long long)ev.steamId, ev.options.PlayerName().c_str());
    // Mirror the parsed player name onto the connection for convenience.
    if (auto conn = GetConnection(ev.clientId)) {
        if (!ev.options.PlayerName().empty()) conn->SetPlayerName(ev.options.PlayerName());
    }
    if (m_clientLoggedInCb) {
        m_clientLoggedInCb(ev);
    } else {
        Logger::Debug("[ConnectionManager::FireClientLoggedIn] no Game subscriber for ClientLoggedIn (client %u)",
                      ev.clientId);
    }
}

void ConnectionManager::FireClientJoined(const ClientJoinedEvent& ev) {
    Logger::Info("[ConnectionManager::FireClientJoined] client %u joined", ev.clientId);
    // The UE3 handshake is complete: from here the game layer may send to this
    // client (until now SendPacket was suppressed to keep the handshake's wire
    // stream clean).
    if (auto conn = GetConnection(ev.clientId)) {
        conn->SetHandshakeComplete(true);
    }

    // Kick off world replication: send the PackageMap export (+ bootstrap actors)
    // so the client can leave the loading screen. This must go out BEFORE the game
    // layer reacts, so the client has the package map before any actor references.
    SendReplicationBootstrap(ev.clientId);

    if (m_clientJoinedCb) {
        m_clientJoinedCb(ev);
    } else {
        Logger::Debug("[ConnectionManager::FireClientJoined] no Game subscriber for ClientJoined (client %u)",
                      ev.clientId);
    }
}

bool ConnectionManager::SendRawToClient(uint32_t clientId, const std::vector<uint8_t>& bytes) {
    auto conn = GetConnection(clientId);
    if (!conn) {
        Logger::Error("[ConnectionManager::SendRawToClient] No connection for client %u (%zu bytes dropped)",
                      clientId, bytes.size());
        return false;
    }
    // `bytes` is a control-channel MESSAGE payload (a ControlChannel::Build*
    // result: <BYTE NMT><fields>). Frame it into UE3 packets - reliable control
    // bunch(es) with a rolling PacketId/ChSequence, fragmented to MaxPacket, with
    // any pending acks drained onto the first packet - then encode and send each.
    // The BunchDataBits SerializeInt bound is phase-dependent on the wire and MUST
    // match what the client decodes with, or the client mis-reads the bunch (it
    // still acks at the packet level, masking the bug). During the StatelessConnect
    // handshake MaxPacket is 8 (bound 64); once the NMT phase begins it is 2048
    // (bound 16384) and large messages (Welcome, PackageMap export) go out as one
    // big bunch rather than 63-bit fragments. Pick both from the handshake state.
    const bool established = GetOrCreateHandshake(clientId).IsControlHandshakeComplete();
    const uint32_t maxPacketBytes = established
        ? PacketCodec::kNmtMaxPacketBytes
        : PacketCodec::kHandshakeMaxPacketBytes;
    const uint32_t maxBunchDataBits = maxPacketBytes * 8u - 1u;

    ControlState& cs = GetControlState(clientId);
    bool ok = true;
    for (const PacketCodec::Packet& pkt :
         cs.outbound.BuildControlMessagePackets(bytes, maxBunchDataBits)) {
        const std::vector<uint8_t> wire = PacketCodec::Encode(pkt, maxPacketBytes);
        if (!conn->SendRaw(wire.data(), wire.size())) {
            ok = false;
        }
    }
    return ok;
}

HandshakeState& ConnectionManager::GetOrCreateHandshake(uint32_t clientId) {
    auto it = m_handshakes.find(clientId);
    if (it != m_handshakes.end()) return *it->second;

    // The handshake's raw-send callback funnels outbound control bytes back to
    // this connection. Capturing `this` + clientId is safe: the handshake is
    // owned by m_handshakes and erased no later than this ConnectionManager.
    auto hs = std::make_unique<HandshakeState>(
        clientId,
        [this, clientId](const std::vector<uint8_t>& payload) {
            this->SendRawToClient(clientId, payload);
        },
        [this](const ClientLoggedInEvent& ev) { this->FireClientLoggedIn(ev); },
        [this](const ClientJoinedEvent& ev)   { this->FireClientJoined(ev); });
    auto* raw = hs.get();
    m_handshakes[clientId] = std::move(hs);
    Logger::Debug("[ConnectionManager::GetOrCreateHandshake] Created handshake for client %u", clientId);
    return *raw;
}

ConnectionManager::ControlState& ConnectionManager::GetControlState(uint32_t clientId) {
    ControlState& cs = m_controlState[clientId];
    if (!cs.reassembler) {
        // Reassembled control messages are dispatched straight into the client's
        // handshake state machine. Capturing `this` + clientId is safe: both maps
        // outlive no later than this ConnectionManager.
        cs.reassembler = std::make_unique<PacketCodec::ControlReassembler>(
            [this, clientId](const std::vector<uint8_t>& payload) {
                this->GetOrCreateHandshake(clientId).HandleControlMessage(payload);
            });
    }
    return cs;
}

void ConnectionManager::SendEncodedPacket(uint32_t clientId, const PacketCodec::Packet& pkt) {
    auto conn = GetConnection(clientId);
    if (!conn) {
        return;
    }
    // Ack-only packets carry no bunches, so the BunchDataBits bound is moot here,
    // but keep the phase-appropriate MaxPacket for consistency with SendRawToClient.
    const bool established = GetOrCreateHandshake(clientId).IsControlHandshakeComplete();
    const uint32_t maxPacketBytes = established
        ? PacketCodec::kNmtMaxPacketBytes
        : PacketCodec::kHandshakeMaxPacketBytes;
    const std::vector<uint8_t> wire = PacketCodec::Encode(pkt, maxPacketBytes);
    conn->SendRaw(wire.data(), wire.size());
}

namespace {
// Load the replication-bootstrap record stream once and cache it. Format:
// repeated [uint32 LE length][length payload bytes]. Each record is one complete
// control-channel message payload (e.g. an NMT 0x07 PackageMap chunk) to send as
// one reliable control bunch. Returns an empty vector if the file is absent/empty
// (replication simply doesn't run - the handshake itself is unaffected).
const std::vector<std::vector<uint8_t>>& GetReplicationBootstrapRecords() {
    static std::once_flag once;
    static std::vector<std::vector<uint8_t>> records;
    std::call_once(once, [] {
        const char* kPath = "data/replication_bootstrap.bin";
        std::ifstream f(kPath, std::ios::binary);
        if (!f) {
            Logger::Info("[ReplicationBootstrap] '%s' not present - post-Join replication disabled",
                         kPath);
            return;
        }
        std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        size_t off = 0;
        while (off + 4 <= all.size()) {
            const uint32_t len = static_cast<uint32_t>(all[off]) |
                                 (static_cast<uint32_t>(all[off + 1]) << 8) |
                                 (static_cast<uint32_t>(all[off + 2]) << 16) |
                                 (static_cast<uint32_t>(all[off + 3]) << 24);
            off += 4;
            if (len == 0 || off + len > all.size()) {
                Logger::Warn("[ReplicationBootstrap] truncated/invalid record at offset %zu (len=%u) - stopping",
                             off - 4, len);
                break;
            }
            records.emplace_back(all.begin() + off, all.begin() + off + len);
            off += len;
        }
        Logger::Info("[ReplicationBootstrap] loaded %zu records (%zu bytes) from '%s'",
                     records.size(), all.size(), kPath);
    });
    return records;
}
} // namespace

void ConnectionManager::SendReplicationBootstrap(uint32_t clientId) {
    const std::vector<std::vector<uint8_t>>& records = GetReplicationBootstrapRecords();
    if (records.empty()) {
        return;
    }
    Logger::Info("[ConnectionManager::SendReplicationBootstrap] client %u: sending %zu replication bootstrap messages",
                 clientId, records.size());
    size_t sent = 0;
    for (const std::vector<uint8_t>& msg : records) {
        if (SendRawToClient(clientId, msg)) {
            ++sent;
        }
    }
    Logger::Info("[ConnectionManager::SendReplicationBootstrap] client %u: sent %zu/%zu messages",
                 clientId, sent, records.size());
}

bool ConnectionManager::ParseIncomingControl(uint32_t clientId, const std::vector<uint8_t>& datagram) {
    Logger::Trace("[ConnectionManager::ParseIncomingControl] Entry: client=%u, %zu bytes",
                  clientId, datagram.size());
    if (datagram.empty()) {
        Logger::Trace("[ConnectionManager::ParseIncomingControl] empty datagram, ignoring");
        return false;
    }

    // Decode the UE3 packet framing (PacketId, acks, bunches). MaxPacket (which
    // sets the BunchDataBits SerializeInt bound) is phase-dependent: 8 during the
    // StatelessConnect handshake, ~512 once the NMT phase begins. Pick it from the
    // connection's handshake state. See docs/RS2V_ControlChannel_WireSpec_7258.md.
    const uint32_t maxPacketBytes =
        GetOrCreateHandshake(clientId).IsControlHandshakeComplete()
            ? PacketCodec::kNmtMaxPacketBytes
            : PacketCodec::kHandshakeMaxPacketBytes;
    PacketCodec::Packet pkt =
        PacketCodec::Decode(datagram.data(), datagram.size(), maxPacketBytes);
    if (!pkt.ok) {
        Logger::Debug("[ConnectionManager::ParseIncomingControl] client %u: %zu bytes are not a decodable UE3 packet, ignoring",
                      clientId, datagram.size());
        return false;
    }

    // This peer speaks UE3: mark it so the legacy emulator-Packet send path is
    // suppressed for it (a UE3 client mis-parses that format - see SendPacket).
    if (auto conn = GetConnection(clientId)) {
        conn->SetUE3Client(true);
    }

    ControlState& cs = GetControlState(clientId);

    // Acknowledge this received packet ONLY if it carried bunch data. Acking a
    // pure-ack packet would make the peer ack our ack, and us ack that, forever
    // (an infinite ack ping-pong with no data - observed against the live client).
    // UE3 only acks packets that delivered bunches. The ack rides on the next
    // outbound packet (e.g. the handshake response), or a standalone ack below.
    if (!pkt.bunches.empty()) {
        cs.outbound.QueueAck(pkt.packetId);
    }

    // (pkt.acks confirm OUR reliable bunches arrived; with the handshake's
    // request/response pacing there is no retransmit window to advance yet, so
    // they are currently informational.)

    // Feed control-channel bunches to the reassembler. Complete messages are
    // dispatched to the handshake, which may emit responses via SendRawToClient
    // (draining the queued ack onto the response packet).
    for (const PacketCodec::Bunch& b : pkt.bunches) {
        cs.reassembler->OnBunch(b);
    }

    // If nothing we sent carried the ack, emit a standalone ack so the client
    // advances its reliable window (e.g. while a multi-bunch Hello is still
    // being received).
    if (cs.outbound.PendingAckCount() > 0) {
        SendEncodedPacket(clientId, cs.outbound.BuildAckOnlyPacket());
    }

    Logger::Trace("[ConnectionManager::ParseIncomingControl] Exit: client=%u now in state %s",
                  clientId, HandshakePhaseName(GetOrCreateHandshake(clientId).Phase()));
    return true;
}
