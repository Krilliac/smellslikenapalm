// src/Network/ConnectionManager.cpp

#include "Network/ConnectionManager.h"
#include "Game/GameServer.h"
#include "Game/TeamManager.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "Network/BitWriter.h"
#include "Network/BitReader.h"
#include "../../telemetry/TelemetryManager.h"
#include <chrono>
#include <fstream>
#include <mutex>
#include <algorithm>

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

        // Defensive: recvfrom must never report more than the buffer it was handed,
        // but clamp before resize() so a misbehaving socket layer can't make us grow
        // the buffer past the bytes actually written (which would expose uninitialized
        // memory to the parse path). Valid datagrams are always <= buffer.size().
        if (len > static_cast<int>(buffer.size())) {
            Logger::Warn("[ConnectionManager::PumpNetwork] ReceiveFrom reported %d bytes > buffer %zu from %s:%u, clamping",
                         len, buffer.size(), srcIp.c_str(), srcPort);
            len = static_cast<int>(buffer.size());
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

    // Resend any reliable bunches the client hasn't acked within the timeout. Runs every
    // pump cycle (the poll loop is tight) so a dropped bootstrap open is recovered in
    // ~250ms instead of stalling the channel forever (the soft-lock).
    RetransmitTick();

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

    // HANDSHAKE WIRE TRACE: dump small inbound datagrams (the handshake/control
    // packets are tiny) so we can see exactly what the real client sends. Skipped
    // for large datagrams to avoid spamming gameplay traffic.
    if (data.size() <= 80) {
        std::string hex; hex.reserve(data.size() * 2);
        static const char* H = "0123456789abcdef";
        for (uint8_t b : data) { hex += H[b >> 4]; hex += H[b & 0xF]; }
        Logger::Debug("[WIRE<-] client %u %zuB: %s", clientId, data.size(), hex.c_str());
    }

    // CreateOrGetClient above guarantees this entry exists, but look it up with a
    // checked find() rather than operator[]: operator[] would silently default-insert
    // a null shared_ptr if the entry were ever missing, and the very next line would
    // dereference it. Reject safely instead of crashing on attacker traffic.
    auto connIt = m_clients.find(addr);
    if (connIt == m_clients.end() || !connIt->second) {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] no connection object for %s:%u (clientId=%u), dropping packet",
                     addr.ip.c_str(), addr.port, clientId);
        return;
    }
    auto conn = connIt->second;
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

    // World-replication bootstrap goes out HERE - immediately after NMT_Welcome -
    // exactly as the official server does (capture: Welcome f162 -> PackageMap
    // f167-f185, BEFORE the client's "packages verified" reply at f227). Sending it
    // on Join instead would deadlock a real client: it won't send Join/ready until
    // it has reconciled the PackageMap. See docs/RS2V_PostJoin_Replication_7258.md.
    SendReplicationBootstrap(ev.clientId);

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
    // The PackageMap export went out earlier (on ClientLoggedIn / right after
    // Welcome). Now that the client has Joined, open the bootstrap ACTOR channels
    // (ROGameReplicationInfo, TeamInfo, the local PlayerController, PRIs) so it can
    // build the world and spawn. Best-effort verbatim replay of the official f231
    // burst - see SendActorBootstrap.
    SendActorBootstrap(ev.clientId);

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
    // We are the SERVER: encode S2C bunches with the server's MaxPacket (~1500,
    // bound ~12000) from the FIRST packet (including the HandshakeChallenge) - the
    // client decodes server bunches at that bound from the start. (Asymmetric vs the
    // client's 2048 that we DECODE inbound with - see PacketCodec.h. There is NO
    // small-bound handshake phase; the old bound-64 encode made our challenge
    // unparseable to the real client, stalling it on the loading screen.)
    const uint32_t maxPacketBytes = PacketCodec::kServerSendMaxPacketBytes;
    const uint32_t maxBunchDataBits = maxPacketBytes * 8u - 1u;

    ControlState& cs = GetControlState(clientId);
    bool ok = true;
    for (const PacketCodec::Packet& pkt :
         cs.outbound.BuildControlMessagePackets(bytes, maxBunchDataBits)) {
        const std::vector<uint8_t> wire = PacketCodec::Encode(pkt, maxPacketBytes);
        if (wire.size() <= 80) {  // HANDSHAKE WIRE TRACE (small control sends)
            std::string hex; hex.reserve(wire.size() * 2);
            static const char* H = "0123456789abcdef";
            for (uint8_t b : wire) { hex += H[b >> 4]; hex += H[b & 0xF]; }
            Logger::Debug("[WIRE->] client %u %zuB: %s", clientId, wire.size(), hex.c_str());
        }
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
    // but keep the server-send MaxPacket for consistency (always, no phase).
    const std::vector<uint8_t> wire =
        PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
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

namespace {
// One bootstrap actor bunch descriptor parsed from data/actor_bootstrap.bin.
struct ActorBunchRecord {
    uint16_t chIndex = 0;
    uint8_t  chType = 0;
    bool     bOpen = false, bClose = false, bReliable = false, bControl = false;
    uint16_t chSequence = 0;
    uint32_t bunchDataBits = 0;       // EXACT bit count (payload is ceil/8 bytes)
    std::vector<uint8_t> payload;
};

const std::vector<ActorBunchRecord>& GetActorBootstrapRecords() {
    static std::once_flag once;
    static std::vector<ActorBunchRecord> records;
    std::call_once(once, [] {
        const char* kPath = "data/actor_bootstrap.bin";
        std::ifstream f(kPath, std::ios::binary);
        if (!f) {
            Logger::Info("[ActorBootstrap] '%s' not present - bootstrap actor channels disabled", kPath);
            return;
        }
        std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        size_t off = 0;
        // record: u16 chIndex | u8 chType | u8 flags | u16 chSeq | u32 len | payload
        while (off + 10 <= all.size()) {
            ActorBunchRecord r;
            r.chIndex = static_cast<uint16_t>(all[off] | (all[off + 1] << 8));
            r.chType = all[off + 2];
            const uint8_t flags = all[off + 3];
            r.bOpen = flags & 0x1; r.bClose = flags & 0x2;
            r.bReliable = flags & 0x4; r.bControl = flags & 0x8;
            r.chSequence = static_cast<uint16_t>(all[off + 4] | (all[off + 5] << 8));
            r.bunchDataBits = static_cast<uint32_t>(all[off + 6]) |
                              (static_cast<uint32_t>(all[off + 7]) << 8) |
                              (static_cast<uint32_t>(all[off + 8]) << 16) |
                              (static_cast<uint32_t>(all[off + 9]) << 24);
            const size_t len = (r.bunchDataBits + 7) / 8;  // payload bytes
            off += 10;
            if (off + len > all.size()) {
                Logger::Warn("[ActorBootstrap] truncated record (bits=%u) - stopping", r.bunchDataBits);
                break;
            }
            r.payload.assign(all.begin() + off, all.begin() + off + len);
            off += len;
            records.push_back(std::move(r));
        }
        Logger::Info("[ActorBootstrap] loaded %zu actor bunch descriptors from '%s'",
                     records.size(), kPath);
    });
    return records;
}
} // namespace

void ConnectionManager::SendActorBootstrap(uint32_t clientId) {
    const std::vector<ActorBunchRecord>& records = GetActorBootstrapRecords();
    if (records.empty()) {
        return;
    }
    auto conn = GetConnection(clientId);
    if (!conn) {
        return;
    }
    ControlState& cs = GetControlState(clientId);

    // Match the real server's pre-actor control sequence. In the capture the official
    // server sends one NMT 0x24 message (payload int32 LE = 1; bytes 24 01 00 00 00) on
    // the control channel at f1484, immediately AFTER the client's Join and immediately
    // BEFORE it opens any actor channels. Our flow previously went Join -> Joined ->
    // actor opens with nothing in between. Diagnosis from server_live.log: the retail
    // client KEEPS its own PlayerController (ch2, NetPlayerIndex==0) but tears down every
    // other bootstrap actor channel (ch3..ch140) with empty bClose bunches and NO
    // NMT_ActorChannelFailure - i.e. the actors spawn then get torn down (a state gate),
    // not a class-resolution or encoding failure. NMT 0x24 is the one control message the
    // real pre-actor sequence has that ours lacked; send it first and let the live client
    // tell us whether it is the missing state transition. See .remember/remember.md.
    static const std::vector<uint8_t> kPreActorNmt24 = {0x24, 0x01, 0x00, 0x00, 0x00};
    SendRawToClient(clientId, kPreActorNmt24);

    Logger::Info("[ConnectionManager::SendActorBootstrap] client %u: NMT 0x24 sent; opening %zu bootstrap actor channels",
                 clientId, records.size());
    // BATCH the actor opens into MaxPacket-sized packets instead of one datagram each.
    // Sending 139 back-to-back single-bunch datagrams overflows the client's UDP receive
    // buffer (even on loopback) and intermittently drops the ch2 open, so the client's
    // PlayerController never binds (no menu). Packing ~10-14 opens per ~1500-byte packet
    // cuts the burst to ~12 packets and makes binding reliable - and matches how the real
    // server frames its open burst (multiple bunches per packet).
    std::vector<PacketCodec::Bunch> batch;
    size_t batchBits = 0;
    // Budget under the 1500-byte server MaxPacket: ~1400 bytes = 11200 bits, minus a
    // per-bunch header allowance (~64 bits) folded into the running estimate below.
    constexpr size_t kBatchBitBudget = 11000;
    auto flushBatch = [&]() {
        if (batch.empty()) return;
        SendReliableBunches(clientId, batch);  // sent + recorded for retransmission
        batch.clear();
        batchBits = 0;
    };
    // Deliver the PlayerController channel (ch2) FIRST, in its own packet, before the
    // rest of the flood. The client adopts ch2 (NetPlayerIndex==0) as its LOCAL
    // PlayerController via HandleClientPlayer - and the team menu only opens when that
    // adoption succeeds (ShowTeamSelect's LocalPlayer(Player)!=none gate). Burying the
    // ch2 open in the middle of 138 other opens makes the adoption intermittent; giving
    // it a clean, standalone packet up front makes it reliable.
    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2) {
            PacketCodec::Bunch pcb;
            pcb.bControl = r.bControl; pcb.bOpen = r.bOpen; pcb.bClose = r.bClose;
            pcb.bReliable = r.bReliable; pcb.chIndex = r.chIndex; pcb.chType = r.chType;
            pcb.chSequence = r.chSequence; pcb.payload = r.payload;
            pcb.payloadBits = r.bunchDataBits;
            SendReliableBunches(clientId, { pcb });  // ch2 standalone, recorded for retransmit
            break;
        }
    }

    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2) continue;   // already sent first, standalone
        if (r.chIndex == 0) {
            // A ch0 control bunch in the burst rides the normal control path; flush the
            // pending actor batch first so ordering is preserved.
            flushBatch();
            SendRawToClient(clientId, r.payload);
            continue;
        }
        PacketCodec::Bunch b;
        b.bControl = r.bControl;
        b.bOpen = r.bOpen;
        b.bClose = r.bClose;
        b.bReliable = r.bReliable;
        b.chIndex = r.chIndex;
        b.chType = r.chType;
        b.chSequence = r.chSequence;
        b.payload = r.payload;
        b.payloadBits = r.bunchDataBits;  // exact bit count (not byte-padded)
        const size_t est = r.bunchDataBits + 64;  // payload + bunch-header allowance
        if (batchBits + est > kBatchBitBudget) {
            flushBatch();
        }
        batch.push_back(std::move(b));
        batchBits += est;
    }
    flushBatch();

    // ---- Open the team-select menu: ClientShowTeamSelect() on ch2 (the PC) -----
    // The live retail client reaches the world but sits in spectator/preload with no
    // team. ROGameInfo.uc:2631 shows the real server calls ROPC.ClientShowTeamSelect()
    // on a fresh joiner to pop the team-select menu (the if-branch at 2621 is
    // ChangedTeams() for players who already have a team). ClientShowTeamSelect is a
    // `reliable client` function taking NO parameters, so the actor-channel bunch body
    // is exactly one field handle: SerializeInt(handle, maxHandle), nothing after.
    // ShowTeamSelect() is safe against our opens-only (empty) GRI: its server-only
    // guard (WorldInfo.Game!=none) is false on the client, it opens the scene from the
    // default TeamSelectSceneTemplate, and InitTeamSelect tolerates an empty
    // GRI.Teams (None.NumPlayers == 0 in UnrealScript). See ROPlayerController.uc:5818.
    //
    // handle / maxHandle from the compiled .u packages via UELib, sorted by the real
    // engine NetIndex (tools/netfields_from_u.ps1) - this is GROUND TRUTH, not the
    // decompiled .uc declaration order (which is reordered and gave a wrong handle).
    // Triple-confirmed: (1) NetIndex sort -> handle 206; (2) decoding the real-server
    // capture's 20571 S2C ch2 bunches with this map yields ZERO Server-function
    // mismatches; (3) SerializeInt(206,531) = bytes CE 00 = the exact `ce00` bunch the
    // official server sends at capture frame f1637 (its own ClientShowTeamSelect).
    constexpr uint32_t kROPlayerControllerMaxHandle = 531;
    constexpr uint32_t kClientShowTeamSelectHandle  = 206;

    const ActorBunchRecord* pcRec = nullptr;
    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2) { pcRec = &r; break; }
    }
    if (pcRec) {
        // Seed ch2's outbound reliable sequence at the open's ChSequence; SendCh2Rpc
        // increments it for each function bunch (ClientShowTeamSelect = seq+1).
        cs.ch2OutReliable = static_cast<uint32_t>(pcRec->chSequence);
        cs.actorChType    = pcRec->chType;
        BitWriter fw;
        fw.SerializeInt(kClientShowTeamSelectHandle, kROPlayerControllerMaxHandle);
        SendCh2Rpc(clientId, fw.GetBytes(), static_cast<uint32_t>(fw.NumBits()),
                   "ClientShowTeamSelect");
    }
}

// PlayerController (ROPlayerController) ClassNetCache maxHandle - see SendActorBootstrap.
static constexpr uint32_t kRoPcMaxHandle = 531;

static uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ConnectionManager::SendReliableBunches(uint32_t clientId,
                                            const std::vector<PacketCodec::Bunch>& bunches) {
    auto conn = GetConnection(clientId);
    if (!conn || bunches.empty()) return;
    ControlState& cs = GetControlState(clientId);
    const PacketCodec::Packet pkt = cs.outbound.BuildRawBunchesPacket(bunches);
    const std::vector<uint8_t> wire =
        PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
    conn->SendRaw(wire.data(), wire.size());
    // Record the reliable bunches so we can retransmit until the client acks this packet.
    std::vector<PacketCodec::Bunch> rel;
    for (const auto& b : bunches) if (b.bReliable) rel.push_back(b);
    if (!rel.empty()) {
        ControlState::SentReliable sr;
        sr.packetIds.push_back(pkt.packetId);
        sr.lastSendMs = NowMs();
        sr.resendCount = 0;
        sr.bunches = std::move(rel);
        cs.pendingReliable.push_back(std::move(sr));
    }
}

void ConnectionManager::OnClientAck(uint32_t clientId, uint32_t ackedPacketId) {
    auto it = m_controlState.find(clientId);
    if (it == m_controlState.end()) return;
    auto& pending = it->second.pendingReliable;
    pending.erase(std::remove_if(pending.begin(), pending.end(),
        [ackedPacketId](const ControlState::SentReliable& sr) {
            for (uint32_t pid : sr.packetIds) if (pid == ackedPacketId) return true;
            return false;
        }), pending.end());
}

void ConnectionManager::RetransmitTick() {
    const uint64_t now = NowMs();
    constexpr uint64_t kRtoMs = 250;     // resend a reliable set un-acked for 250ms
    constexpr int kMaxResends = 12;
    for (auto& kv : m_controlState) {
        ControlState& cs = kv.second;
        if (cs.pendingReliable.empty()) continue;
        auto conn = GetConnection(kv.first);
        if (!conn) continue;
        for (auto& sr : cs.pendingReliable) {
            if (now - sr.lastSendMs < kRtoMs || sr.resendCount >= kMaxResends) continue;
            // Resend the SAME reliable bunches (verbatim, same per-channel ChSequence) in a
            // NEW packet (new PacketId). The client fills the gap or ignores the duplicate.
            const PacketCodec::Packet pkt = cs.outbound.BuildRawBunchesPacket(sr.bunches);
            const std::vector<uint8_t> wire =
                PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
            conn->SendRaw(wire.data(), wire.size());
            sr.packetIds.push_back(pkt.packetId);
            sr.lastSendMs = now;
            ++sr.resendCount;
            Logger::Debug("[ConnectionManager::RetransmitTick] client %u: resent %zu reliable bunch(es) attempt %d (pkt %u)",
                          kv.first, sr.bunches.size(), sr.resendCount, pkt.packetId);
        }
    }
}

void ConnectionManager::SendCh2Rpc(uint32_t clientId, const std::vector<uint8_t>& payload,
                                   uint32_t payloadBits, const char* name) {
    ControlState& cs = GetControlState(clientId);
    PacketCodec::Bunch b;
    b.bControl   = false;
    b.bOpen      = false;
    b.bClose     = false;
    b.bReliable  = true;
    b.chIndex    = 2;
    b.chType     = cs.actorChType;
    b.chSequence = ++cs.ch2OutReliable;   // next reliable on ch2
    b.payload    = payload;
    b.payloadBits = payloadBits;
    Logger::Info("[ConnectionManager::SendCh2Rpc] client %u: sent %s on ch2 seq %u (%u bits)",
                 clientId, name, b.chSequence, payloadBits);
    SendReliableBunches(clientId, { b });   // recorded for retransmission until acked
}

// Names for the handful of ROPlayerController net-field handles we recognise on the
// wire (ground truth from tools/netfields_from_u.ps1). For logging/dispatch only.
static const char* RoPcHandleName(uint32_t h) {
    switch (h) {
        case 170: return "SelectTeam";
        case 172: return "ChangedTeams";
        case 175: return "SelectRoleByClass";
        case 206: return "ClientShowTeamSelect";
        case 207: return "ClientShowRoleSelect";
        case 210: return "ChangedRole";
        case 82:  return "ServerChangeTeam";
        case 80:  return "ServerSuicide";
        case 65:  return "ServerMove";
        case 27:  return "ServerRestartPlayer";
        default:  return "?";
    }
}

void ConnectionManager::DecodeInboundActorBunch(uint32_t clientId,
                                                const PacketCodec::Bunch& bunch) {
    // (Removed the old "proof-of-life re-send" of ClientShowTeamSelect: it sent a NEW
    // bunch at ch2 seq+1, manufacturing a sequence GAP if the original seq was dropped ->
    // ch2 stall -> soft-lock. Reliable retransmission now redelivers the original
    // ClientShowTeamSelect (same ChSequence) until the client acks it.)

    // Only reliable function/property bunches carry a handle worth dispatching; the
    // open/close framing and tiny keepalives don't.
    if (!bunch.bReliable || bunch.bOpen || bunch.bClose || bunch.payloadBits == 0) {
        return;
    }
    BitReader r(bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
    const uint32_t handle = r.SerializeInt(kRoPcMaxHandle);
    if (r.IsOverflowed()) return;
    Logger::Info("[ConnectionManager::DecodeInboundActorBunch] client %u: ch%u inbound RPC handle %u (%s), %u bits",
                 clientId, bunch.chIndex, handle, RoPcHandleName(handle), bunch.payloadBits);

    // SelectTeam(byte TeamID) - the client clicked a team in the team-select menu.
    // ROPlayerController.uc:3440 (reliable server). On the real server this assigns the
    // team then calls ChangedTeams() to open role select. We advance the client to the
    // role-select scene via ClientShowRoleSelect (handle 207, optional bool).
    if (bunch.chIndex == 2 && handle == 170) {
        // UE3 function-call params carry a per-param "Send" PRESENCE BIT for NON-bool
        // params (UnScript.cpp InternalProcessRemoteFunction:2980-3010; receive side
        // UnChan.cpp:1628-1640): read the 1-bit Send flag first; the byte value follows
        // ONLY if Send==1. Send==0 means the value equals its default and is OMITTED.
        // SelectTeam(byte TeamID): TeamID==0 sends Send=0 and NO byte; reading the byte
        // raw would overflow and silently drop the team-0 pick (team-1 previously only
        // "worked" by a &1 masking accident). This is the real team-0 selection bug.
        const bool hasTeamId = r.ReadBit();
        uint8_t teamId = 0;                  // Send==0 -> default 0 (a VALID selection)
        if (hasTeamId) {
            teamId = r.ReadByte();
            if (r.IsOverflowed()) {
                Logger::Warn("[ConnectionManager::DecodeInboundActorBunch] client %u: SelectTeam truncated before TeamID byte, ignoring",
                             clientId);
                return;
            }
        }
        if (teamId > 1) teamId = 1;          // RS2 has two playable teams (0/1)
        ControlState& cs = GetControlState(clientId);
        Logger::Info("[ConnectionManager] client %u: SelectTeam(TeamID=%u) -> JoinTeam (clear spectator + Team) then ChangedTeams",
                     clientId, teamId);
        cs.teamSelected = true;

        // Persist the team SERVER-SIDE (the real JoinTeam result). Previously we only told
        // the CLIENT its team (the ch26 delta + ChangedTeams below) but never updated the
        // authoritative TeamManager - so the server kept the join-time auto-picked team and a
        // player who clicked NVA got the US loadout/spawn (HandleRoleSelection reads
        // TeamManager::GetPlayerTeam). Map RS2 0/1 -> TeamManager 1/2.
        if (m_server) {
            if (auto* tm = m_server->GetTeamManager()) {
                tm->AddPlayerToTeam(clientId, (teamId == 0) ? 1u : 2u);  // also sets Player team
            }
        }

        // --- ADVANCE TO ROLE-SELECT: ChangedTeams (handle 172) on ch2 -------------------
        // ROPlayerController.uc:3533
        //   reliable client simulated function ChangedTeams(byte TeamIndex,
        //       bool bShowRoleSelection, optional Class<GameInfo> GameTypeClass,
        //       optional bool bTeamBalancing, optional bool bShowLobby)
        //
        // The retail client's ChangedTeams() does the team->role transition itself:
        //   * line 3618: PlayerReplicationInfo.Team = WorldInfo.GRI.Teams[TeamIndex]
        //                -> it BINDS PRI.Team from GRI.Teams[] (already populated by our
        //                   TeamInfo opens on ch21/56/76), so NO PRI.Team delta is needed.
        //   * line 3627: ShowRoleSelectScene(GameTypeClass, TeamIndex, ...)
        //                -> uses the GameTypeClass PARAM directly. If it is none, the
        //                   client SKIPS InitSquadsForGametype (ROPlayerController.uc:5941),
        //                   the squad/role tables stay empty, GetSelectedRoleInfoClass()
        //                   returns none, and the role UI does RoleClass.default.X on a
        //                   null class-default object -> EXCEPTION_ACCESS_VIOLATION (the
        //                   VNGame.exe+0xbbf712 crash we saw). The fix is to SEND a real,
        //                   client-resolvable GameTypeClass so squads build locally from
        //                   the client's own loaded ROMapInfo (the map data is NOT
        //                   replicated - confirmed: ROMI = ROMapInfo(WorldInfo.GetMapInfo)).
        //
        // GameTypeClass = ROGame.ROGameInfoTerritories, encoded as the SAME static
        // PackageMap class index the real server already sent as GRI.GameClass (h33) in our
        // verbatim ch54 GRI open: static index 69601 (selector 0). Reusing the exact captured
        // index guarantees it resolves on the client (it already accepted this ref in GRI).
        // See docs/re/CLIENT_CRASH_team_select.md + docs/re/open_bunch_structure.md:185.
        //
        // Param wire layout (UE3 UnScript.cpp:2980-3010, validated against UE3-src):
        //   non-bool param -> [Send presence bit][value iff Send] ; bool -> bare value bit.
        constexpr uint32_t kChangedTeamsHandle           = 172;
        constexpr uint32_t kRoGameInfoTerritoriesClassIx = 69601;  // GRI.GameClass h33 (capture)
        BitWriter fw;
        fw.SerializeInt(kChangedTeamsHandle, kRoPcMaxHandle);      // handle (maxHandle 531)
        // param 1: byte TeamIndex  -> Send only if != default(0)
        if (teamId != 0) { fw.WriteBit(true); fw.WriteByte(teamId); }
        else             { fw.WriteBit(false); }
        // param 2: bool bShowRoleSelection = true  -> open the role-select scene
        fw.WriteBit(true);
        // param 3: Class<GameInfo> GameTypeClass = ROGameInfoTerritories  -> Send + objref
        fw.WriteBit(true);
        // Object-ref (UPackageMapLevel::SerializeObject), STATIC class path: selector bit 0
        // then SerializeInt(index, MAX_OBJECT_INDEX=0x80000000). This is bit-identical to
        // ActorRepl::WriteNetGUID(NetGUIDRef{false,idx}); inlined here to avoid a cross-TU
        // link dependency on src/Network/ActorReplication.cpp, whose obj name collides with
        // src/Protocol/ActorReplication.cpp in rs2v_core (see docs/hardening/HARDENING_LOG.md).
        fw.WriteBit(false);                                            // selector = static
        fw.SerializeInt(kRoGameInfoTerritoriesClassIx, 0x80000000u);   // static class index
        // param 4: bool bTeamBalancing = false
        fw.WriteBit(false);
        // param 5: bool bShowLobby = false
        fw.WriteBit(false);
        SendCh2Rpc(clientId, fw.GetBytes(), static_cast<uint32_t>(fw.NumBits()), "ChangedTeams");
        Logger::Info("[ConnectionManager] client %u: SelectTeam(TeamID=%u) -> sent ChangedTeams "
                     "(role-select advance, GameTypeClass=ROGameInfoTerritories idx %u)",
                     clientId, teamId, kRoGameInfoTerritoriesClassIx);
    }
}

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
    // The client (C2S) frames BunchDataBits at its MaxPacket = 2048 (bound 16384)
    // from the VERY FIRST packet - including the StatelessConnect handshake bunches.
    // There is NO small-bound "handshake phase": decoding the handshake bunches at
    // the old bound 64 misaligned them (the NMT byte landed in the 2nd byte), so the
    // client's HandshakeStart/Response were mis-keyed. Always decode at the NMT bound.
    const uint32_t maxPacketBytes = PacketCodec::kNmtMaxPacketBytes;
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

    // pkt.acks confirm OUR reliable bunches arrived: clear any pending reliable
    // bunch-set that rode in an acked packet so RetransmitTick stops resending it.
    for (uint32_t ackedId : pkt.acks) {
        OnClientAck(clientId, ackedId);
    }

    // Feed control-channel bunches to the reassembler. Complete messages are
    // dispatched to the handshake, which may emit responses via SendRawToClient
    // (draining the queued ack onto the response packet).
    // Per-packet dispatch backstop. The bunch count is already bounded by the
    // datagram size (a single inbound datagram is <= the receive buffer), but cap
    // the dispatch loop explicitly so a pathological packet can't drive an outsized
    // amount of work. The cap is far above any decodable datagram's real bunch count,
    // so valid handshake/bootstrap/actor traffic is never truncated.
    constexpr size_t kMaxBunchesPerPacket = 4096;
    size_t processed = 0;
    for (const PacketCodec::Bunch& b : pkt.bunches) {
        if (++processed > kMaxBunchesPerPacket) {
            Logger::Warn("[ConnectionManager::ParseIncomingControl] client %u: packet %u carried %zu bunches (> cap %zu), dropping remainder",
                         clientId, pkt.packetId, pkt.bunches.size(), kMaxBunchesPerPacket);
            break;
        }
        if (b.chIndex == 0) {
            cs.reassembler->OnBunch(b);          // control channel (handshake/NMT)
        } else {
            DecodeInboundActorBunch(clientId, b);  // ch>=2 actor-channel RPCs (SelectTeam, ...)
        }
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
