// src/Network/ClientConnection.cpp

#include "Network/ClientConnection.h"
#include "Network/ConnectionManager.h"
#include "Network/PacketRecorder.h"
#include "Game/Player.h"
#include "Utils/Logger.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"

ClientConnection::ClientConnection(uint32_t clientId,
                                   const std::string& ip,
                                   uint16_t port,
                                   std::shared_ptr<UDPSocket> socket,
                                   ConnectionManager* manager)
    : m_clientId(clientId)
    , m_ip(ip)
    , m_port(port)
    , m_socket(socket)
    , m_manager(manager)
    , m_lastHeartbeat(std::chrono::steady_clock::now())
    , m_windowStart(std::chrono::steady_clock::now())
{
    Logger::Trace("[ClientConnection::ClientConnection] Entry: clientId=%u, ip='%s', port=%u, socket=%p, manager=%p",
                  clientId, ip.c_str(), port, (void*)socket.get(), (void*)manager);
    Logger::Info("ClientConnection created: ID=%u %s:%u", clientId, ip.c_str(), port);
    Logger::Debug("[ClientConnection::ClientConnection] Heartbeat and window start initialized to current time");
    Logger::Trace("[ClientConnection::ClientConnection] Exit");
}

ClientConnection::~ClientConnection() {
    Logger::Trace("[ClientConnection::~ClientConnection] Entry: clientId=%u, ip='%s', port=%u",
                  m_clientId, m_ip.c_str(), m_port);
    Logger::Debug("[ClientConnection::~ClientConnection] Destroying ClientConnection for client %u (%s:%u), disconnected=%s",
                  m_clientId, m_ip.c_str(), m_port, m_disconnected ? "true" : "false");
    Logger::Trace("[ClientConnection::~ClientConnection] Exit");
}

uint32_t ClientConnection::GetClientId() const {
    Logger::Trace("[ClientConnection::GetClientId] Entry/Exit: returning %u", m_clientId);
    return m_clientId;
}

const std::string& ClientConnection::GetIP() const {
    Logger::Trace("[ClientConnection::GetIP] Entry/Exit: returning '%s'", m_ip.c_str());
    return m_ip;
}

uint16_t ClientConnection::GetPort() const {
    Logger::Trace("[ClientConnection::GetPort] Entry/Exit: returning %u", m_port);
    return m_port;
}

std::string ClientConnection::GetSteamID() const {
    Logger::Trace("[ClientConnection::GetSteamID] Entry: clientId=%u", m_clientId);
    // Prefer the authenticated Steam64 (stamped at login). Fall back to the clientId string
    // only when no real Steam id was resolved, so admin/ban lists keyed on Steam64 can match
    // for clients that present one, without breaking clients that don't.
    if (!m_steamId.empty()) {
        Logger::Trace("[ClientConnection::GetSteamID] Exit: returning authenticated SteamID '%s'", m_steamId.c_str());
        return m_steamId;
    }
    std::string steamId = std::to_string(m_clientId);
    Logger::Debug("[ClientConnection::GetSteamID] No authenticated SteamID; falling back to clientId-derived '%s'", steamId.c_str());
    Logger::Trace("[ClientConnection::GetSteamID] Exit: returning '%s'", steamId.c_str());
    return steamId;
}

void ClientConnection::SetSteamID(const std::string& steamId) {
    Logger::Trace("[ClientConnection::SetSteamID] Entry: clientId=%u, steamId='%s'", m_clientId, steamId.c_str());
    m_steamId = steamId;
    Logger::Debug("[ClientConnection::SetSteamID] client %u authenticated SteamID set to '%s'", m_clientId, steamId.c_str());
}

bool ClientConnection::SendPacket(const Packet& pkt) {
    Logger::Trace("[ClientConnection::SendPacket] Entry: clientId=%u, tag='%s', payloadSize=%u",
                  m_clientId, pkt.GetTag().c_str(), pkt.GetPayloadSize());
    // Gate: NEVER send the emulator's internal Packet format to a UE3-protocol
    // client - not during the handshake and not after Join. A real RS2 client
    // mis-parses these as UE3 packets and corrupts its sequence/ack state (it can
    // disconnect). UE3 clients receive only UE3-framed bytes via SendRaw (control
    // bunches now; world-replication bunches once implemented). The legacy Packet
    // path is reserved for non-UE3 peers (in-process tests/tools). (SendRaw is
    // intentionally not gated.)
    if (m_isUE3Client) {
        Logger::Debug("[ClientConnection::SendPacket] Suppressing legacy game packet tag='%s' to UE3 client %u",
                      pkt.GetTag().c_str(), m_clientId);
        return false;
    }
    // Defensive: a disconnected/torn-down connection may still be referenced by a
    // game-layer caller. Never push bytes after MarkDisconnected, and never deref a
    // null socket (additive guard; correct path always has a live socket).
    if (m_disconnected) {
        Logger::Debug("[ClientConnection::SendPacket] Dropping packet tag='%s' to disconnected client %u",
                      pkt.GetTag().c_str(), m_clientId);
        return false;
    }
    if (!m_socket) {
        Logger::Warn("[ClientConnection::SendPacket] Client %u has null socket, dropping packet tag='%s'",
                     m_clientId, pkt.GetTag().c_str());
        return false;
    }
    auto data = pkt.Serialize();
    Logger::Debug("[ClientConnection::SendPacket] Serialized packet: tag='%s', serialized size=%zu bytes",
                  pkt.GetTag().c_str(), data.size());
    if (!CanSend((uint32_t)data.size())) {
        Logger::Warn("[ClientConnection::SendPacket] Bandwidth limit exceeded for client %u, dropping packet tag='%s' (%zu bytes)",
                     m_clientId, pkt.GetTag().c_str(), data.size());
        Logger::Trace("[ClientConnection::SendPacket] Exit: returning false (bandwidth limit)");
        return false;
    }
    Logger::Debug("[ClientConnection::SendPacket] Bandwidth check passed, sending %zu bytes to %s:%u",
                  data.size(), m_ip.c_str(), m_port);
    bool ok = m_socket->SendTo(m_ip, m_port, data.data(), data.size());
    if (ok) {
        OnBytesSent((uint32_t)data.size());
        Logger::Debug("[ClientConnection::SendPacket] Packet sent successfully: tag='%s', %zu bytes to client %u",
                      pkt.GetTag().c_str(), data.size(), m_clientId);
        // Feed outbound packet to protocol decoder
        GetProtocolDecoder().OnPacketSent(m_clientId, pkt.RawData(), pkt.GetTag());
        Logger::Debug("[ClientConnection::SendPacket] Fed outbound packet to protocol decoder");
    } else {
        Logger::Error("[ClientConnection::SendPacket] Failed to send packet tag='%s' (%zu bytes) to client %u (%s:%u)",
                      pkt.GetTag().c_str(), data.size(), m_clientId, m_ip.c_str(), m_port);
    }
    Logger::Trace("[ClientConnection::SendPacket] Exit: returning %s", ok ? "true" : "false");
    return ok;
}

bool ClientConnection::SendRaw(const uint8_t* data, size_t len) {
    Logger::Trace("[ClientConnection::SendRaw] Entry: clientId=%u, len=%zu", m_clientId, len);
    if (!m_socket || !data || len == 0) {
        Logger::Warn("[ClientConnection::SendRaw] Client %u: invalid socket/data, dropping %zu bytes",
                     m_clientId, len);
        return false;
    }
    // Never push bytes after MarkDisconnected(): admin kick/ban and failed
    // PreLogin mark the connection disconnected without immediately erasing it,
    // and reliable retransmits / late bootstrap responses ride this raw path.
    // Mirror SendPacket's guard so a kicked or rejected client stops receiving
    // server traffic.
    if (m_disconnected) {
        Logger::Debug("[ClientConnection::SendRaw] Dropping %zu raw bytes to disconnected client %u",
                      len, m_clientId);
        return false;
    }
    if (!CanSend((uint32_t)len)) {
        Logger::Warn("[ClientConnection::SendRaw] Bandwidth limit exceeded for client %u, dropping %zu raw bytes",
                     m_clientId, len);
        return false;
    }
    bool ok = m_socket->SendTo(m_ip, m_port, data, len);
    if (ok) {
        OnBytesSent((uint32_t)len);
        // GLOBAL PACKET RECORDER: every outbound datagram (S2C) -> packetlog/ sniff.
        net::PacketRecorder::Instance().RecordDatagram(
            net::PktDir::S2C, m_clientId, m_ip + ":" + std::to_string(m_port), data, len);
        Logger::Debug("[ClientConnection::SendRaw] Sent %zu raw bytes to client %u (%s:%u)",
                      len, m_clientId, m_ip.c_str(), m_port);
    } else {
        Logger::Error("[ClientConnection::SendRaw] Failed to send %zu raw bytes to client %u (%s:%u)",
                      len, m_clientId, m_ip.c_str(), m_port);
    }
    Logger::Trace("[ClientConnection::SendRaw] Exit: returning %s", ok ? "true" : "false");
    return ok;
}

bool ClientConnection::ReceiveRaw(std::vector<uint8_t>& outData, PacketMetadata& meta) {
    Logger::Trace("[ClientConnection::ReceiveRaw] Entry: clientId=%u", m_clientId);
    // Defensive: never deref a null socket on the receive path (additive guard).
    if (!m_socket) {
        Logger::Warn("[ClientConnection::ReceiveRaw] Client %u has null socket, no data", m_clientId);
        return false;
    }
    const int kRecvBufSize = 1500;
    outData.resize(kRecvBufSize);
    std::string fromIp;
    uint16_t fromPort = 0;
    Logger::Debug("[ClientConnection::ReceiveRaw] Receiving from socket, buffer size=%d", kRecvBufSize);
    int len = m_socket->ReceiveFrom(fromIp, fromPort, outData.data(), (int)outData.size());
    if (len <= 0) {
        Logger::Debug("[ClientConnection::ReceiveRaw] ReceiveFrom returned %d, no data available for client %u", len, m_clientId);
        Logger::Trace("[ClientConnection::ReceiveRaw] Exit: returning false (no data)");
        return false;
    }
    // Defensive: do NOT trust the returned length. ReceiveFrom only wrote up to
    // kRecvBufSize bytes into the buffer; if it reports more (driver/impl bug),
    // resizing to that value would expose value-initialized tail bytes as if they
    // were received packet data. Clamp to what the buffer can actually hold.
    if (len > kRecvBufSize) {
        Logger::Warn("[ClientConnection::ReceiveRaw] Client %u: ReceiveFrom reported %d bytes > buffer %d, clamping",
                     m_clientId, len, kRecvBufSize);
        len = kRecvBufSize;
    }
    outData.resize(len);
    Logger::Debug("[ClientConnection::ReceiveRaw] Received %d bytes from %s:%u for client %u",
                  len, fromIp.c_str(), fromPort, m_clientId);
    m_lastRaw = outData;
    m_lastPacket = Packet::FromBuffer(outData, meta);
    Logger::Debug("[ClientConnection::ReceiveRaw] Parsed packet: tag='%s', stored as lastPacket and lastRaw",
                  m_lastPacket.GetTag().c_str());
    Logger::Trace("[ClientConnection::ReceiveRaw] Exit: returning true");
    return true;
}

Packet ClientConnection::LastPacket() const {
    Logger::Trace("[ClientConnection::LastPacket] Entry/Exit: clientId=%u, tag='%s'",
                  m_clientId, m_lastPacket.GetTag().c_str());
    return m_lastPacket;
}

std::vector<uint8_t> ClientConnection::LastRawPacket() const {
    Logger::Trace("[ClientConnection::LastRawPacket] Entry/Exit: clientId=%u, raw size=%zu",
                  m_clientId, m_lastRaw.size());
    return m_lastRaw;
}

void ClientConnection::MarkDisconnected() {
    Logger::Trace("[ClientConnection::MarkDisconnected] Entry: clientId=%u, previous state=%s",
                  m_clientId, m_disconnected ? "disconnected" : "connected");
    m_disconnected = true;
    Logger::Info("[ClientConnection::MarkDisconnected] Client %u (%s:%u) marked as disconnected",
                 m_clientId, m_ip.c_str(), m_port);
    Logger::Trace("[ClientConnection::MarkDisconnected] Exit");
}

bool ClientConnection::IsDisconnected() const {
    Logger::Trace("[ClientConnection::IsDisconnected] Entry/Exit: clientId=%u, disconnected=%s",
                  m_clientId, m_disconnected ? "true" : "false");
    return m_disconnected;
}

void ClientConnection::UpdateLastHeartbeat() {
    Logger::Trace("[ClientConnection::UpdateLastHeartbeat] Entry: clientId=%u", m_clientId);
    m_lastHeartbeat = std::chrono::steady_clock::now();
    Logger::Debug("[ClientConnection::UpdateLastHeartbeat] Heartbeat updated for client %u (%s:%u)",
                  m_clientId, m_ip.c_str(), m_port);
    Logger::Trace("[ClientConnection::UpdateLastHeartbeat] Exit");
}

std::chrono::steady_clock::time_point ClientConnection::GetLastHeartbeat() const {
    Logger::Trace("[ClientConnection::GetLastHeartbeat] Entry/Exit: clientId=%u", m_clientId);
    return m_lastHeartbeat;
}

// Player name/team management

void ClientConnection::SetPlayerName(const std::string& name) {
    Logger::Trace("[ClientConnection::SetPlayerName] Entry: clientId=%u, name='%s'", m_clientId, name.c_str());
    std::string previousName = m_playerName;
    m_playerName = name;
    Logger::Debug("[ClientConnection::SetPlayerName] Client %u player name changed: '%s' -> '%s'",
                  m_clientId, previousName.c_str(), m_playerName.c_str());
    Logger::Info("[ClientConnection::SetPlayerName] Client %u now known as '%s'", m_clientId, name.c_str());
    Logger::Trace("[ClientConnection::SetPlayerName] Exit");
}

const std::string& ClientConnection::GetPlayerName() const {
    Logger::Trace("[ClientConnection::GetPlayerName] Entry/Exit: clientId=%u, name='%s'",
                  m_clientId, m_playerName.c_str());
    return m_playerName;
}

uint32_t ClientConnection::GetTeamId() const {
    Logger::Trace("[ClientConnection::GetTeamId] Entry/Exit: clientId=%u, teamId=%u", m_clientId, m_teamId);
    return m_teamId;
}

void ClientConnection::SetTeamId(uint32_t teamId) {
    Logger::Trace("[ClientConnection::SetTeamId] Entry: clientId=%u, teamId=%u", m_clientId, teamId);
    uint32_t previousTeamId = m_teamId;
    m_teamId = teamId;
    Logger::Debug("[ClientConnection::SetTeamId] Client %u team changed: %u -> %u",
                  m_clientId, previousTeamId, m_teamId);
    Logger::Info("[ClientConnection::SetTeamId] Client %u assigned to team %u", m_clientId, teamId);
    Logger::Trace("[ClientConnection::SetTeamId] Exit");
}

// Game-layer helpers

void ClientConnection::SendInventoryUpdate(const std::vector<InventoryItem>& items) {
    Logger::Trace("[ClientConnection::SendInventoryUpdate] Entry: clientId=%u, items count=%zu",
                  m_clientId, items.size());
    Packet p("INVENTORY_UPDATE");
    p.WriteUInt(static_cast<uint32_t>(items.size()));
    Logger::Debug("[ClientConnection::SendInventoryUpdate] Writing %zu inventory items to packet", items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        p.WriteString(item.name);
        p.WriteInt(item.quantity);
        Logger::Trace("[ClientConnection::SendInventoryUpdate] Item[%zu]: name='%s', quantity=%d",
                      i, item.name.c_str(), item.quantity);
    }
    Logger::Debug("[ClientConnection::SendInventoryUpdate] Sending INVENTORY_UPDATE with %zu items to client %u",
                  items.size(), m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendInventoryUpdate] Exit");
}

void ClientConnection::SendChatMessage(const std::string& msg) {
    Logger::Trace("[ClientConnection::SendChatMessage] Entry: clientId=%u, msg='%s'", m_clientId, msg.c_str());
    Packet p("CHAT_MESSAGE");
    p.WriteString(msg);
    Logger::Debug("[ClientConnection::SendChatMessage] Sending CHAT_MESSAGE (length=%zu) to client %u",
                  msg.size(), m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendChatMessage] Exit");
}

void ClientConnection::SendPositionUpdate(const Vector3& pos) {
    Logger::Trace("[ClientConnection::SendPositionUpdate] Entry: clientId=%u, pos=(%f, %f, %f)",
                  m_clientId, pos.x, pos.y, pos.z);
    Packet p("PLAYER_POSITION");
    p.WriteVector3(pos);
    Logger::Debug("[ClientConnection::SendPositionUpdate] Sending PLAYER_POSITION (%f, %f, %f) to client %u",
                  pos.x, pos.y, pos.z, m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendPositionUpdate] Exit");
}

void ClientConnection::SendOrientationUpdate(const Vector3& dir) {
    Logger::Trace("[ClientConnection::SendOrientationUpdate] Entry: clientId=%u, dir=(%f, %f, %f)",
                  m_clientId, dir.x, dir.y, dir.z);
    Packet p("PLAYER_ORIENTATION");
    p.WriteVector3(dir);
    Logger::Debug("[ClientConnection::SendOrientationUpdate] Sending PLAYER_ORIENTATION (%f, %f, %f) to client %u",
                  dir.x, dir.y, dir.z, m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendOrientationUpdate] Exit");
}

void ClientConnection::SendHealthUpdate(int hp) {
    Logger::Trace("[ClientConnection::SendHealthUpdate] Entry: clientId=%u, hp=%d", m_clientId, hp);
    Packet p("HEALTH_UPDATE");
    p.WriteInt(hp);
    Logger::Debug("[ClientConnection::SendHealthUpdate] Sending HEALTH_UPDATE hp=%d to client %u", hp, m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendHealthUpdate] Exit");
}

void ClientConnection::SendTeamUpdate(uint32_t teamId) {
    Logger::Trace("[ClientConnection::SendTeamUpdate] Entry: clientId=%u, teamId=%u", m_clientId, teamId);
    Packet p("TEAM_UPDATE");
    p.WriteUInt(teamId);
    Logger::Debug("[ClientConnection::SendTeamUpdate] Sending TEAM_UPDATE teamId=%u to client %u", teamId, m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendTeamUpdate] Exit");
}

void ClientConnection::SendSpawnPlayer() {
    Logger::Trace("[ClientConnection::SendSpawnPlayer] Entry: clientId=%u", m_clientId);
    Packet p("SPAWN_PLAYER");
    Logger::Debug("[ClientConnection::SendSpawnPlayer] Sending SPAWN_PLAYER to client %u", m_clientId);
    SendPacket(p);
    Logger::Info("[ClientConnection::SendSpawnPlayer] Spawn player packet sent to client %u", m_clientId);
    Logger::Trace("[ClientConnection::SendSpawnPlayer] Exit");
}

void ClientConnection::SendSessionState(uint32_t aliveCount) {
    Logger::Trace("[ClientConnection::SendSessionState] Entry: clientId=%u, aliveCount=%u", m_clientId, aliveCount);
    Packet p("SESSION_STATE");
    p.WriteUInt(aliveCount);
    Logger::Debug("[ClientConnection::SendSessionState] Sending SESSION_STATE aliveCount=%u to client %u",
                  aliveCount, m_clientId);
    SendPacket(p);
    Logger::Trace("[ClientConnection::SendSessionState] Exit");
}

// Bandwidth / rate limiting

bool ClientConnection::CanSend(uint32_t byteCount) {
    Logger::Trace("[ClientConnection::CanSend] Entry: clientId=%u, byteCount=%u", m_clientId, byteCount);
    ResetWindowIfNeeded();
    // Defensive: never deref a null manager. The correct path always has a manager;
    // if it is somehow null we cannot consult the limit, so allow the send rather
    // than silently dropping a valid client's traffic (additive, non-fatal).
    if (!m_manager) {
        Logger::Warn("[ClientConnection::CanSend] Client %u has null manager, skipping bandwidth check", m_clientId);
        return true;
    }
    uint32_t limit = m_manager->GetBandwidthLimit();
    // Defensive: compute without integer overflow. m_sentBytesThisWindow + byteCount
    // can wrap a uint32_t (byteCount is a size_t cast at the call site), and a wrapped
    // sum could falsely pass the cap. Subtraction-form comparison cannot overflow.
    bool canSend = (byteCount <= limit) && (m_sentBytesThisWindow <= limit - byteCount);
    Logger::Debug("[ClientConnection::CanSend] Client %u: sentBytesThisWindow=%u, requested=%u, limit=%u, canSend=%s",
                  m_clientId, m_sentBytesThisWindow, byteCount, limit, canSend ? "true" : "false");
    if (!canSend) {
        Logger::Warn("[ClientConnection::CanSend] Client %u would exceed bandwidth limit: %u + %u > %u",
                     m_clientId, m_sentBytesThisWindow, byteCount, limit);
    }
    Logger::Trace("[ClientConnection::CanSend] Exit: returning %s", canSend ? "true" : "false");
    return canSend;
}

void ClientConnection::OnBytesSent(uint32_t byteCount) {
    Logger::Trace("[ClientConnection::OnBytesSent] Entry: clientId=%u, byteCount=%u", m_clientId, byteCount);
    ResetWindowIfNeeded();
    uint32_t previousSent = m_sentBytesThisWindow;
    // Defensive: saturating add. If the window counter ever overflowed it would wrap
    // to a small value and defeat the bandwidth cap, letting a stuck/abusive client
    // resume flooding. Clamp at UINT32_MAX instead (additive guard).
    if (m_sentBytesThisWindow > UINT32_MAX - byteCount) {
        m_sentBytesThisWindow = UINT32_MAX;
    } else {
        m_sentBytesThisWindow += byteCount;
    }
    Logger::Debug("[ClientConnection::OnBytesSent] Client %u: sentBytesThisWindow %u -> %u",
                  m_clientId, previousSent, m_sentBytesThisWindow);
    Logger::Trace("[ClientConnection::OnBytesSent] Exit");
}

void ClientConnection::ResetWindowIfNeeded() {
    Logger::Trace("[ClientConnection::ResetWindowIfNeeded] Entry: clientId=%u", m_clientId);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_windowStart).count();
    // Defensive: guard against time going backwards. steady_clock should be
    // monotonic, but if m_windowStart ever ends up ahead of now (clock anomaly,
    // bad construction order) elapsed is negative and the window would NEVER reset,
    // permanently wedging this client's bandwidth counter and blocking all sends.
    // Treat a backwards jump as window expiry and re-anchor (additive, non-fatal).
    if (elapsed < 0) {
        Logger::Warn("[ClientConnection::ResetWindowIfNeeded] Client %u: time went backwards (elapsed=%lld s), re-anchoring window",
                     m_clientId, (long long)elapsed);
        m_windowStart = now;
        m_sentBytesThisWindow = 0;
    } else if (elapsed >= 1) {
        Logger::Debug("[ClientConnection::ResetWindowIfNeeded] Client %u: window expired (elapsed=%lld s), resetting sentBytesThisWindow=%u to 0",
                      m_clientId, (long long)elapsed, m_sentBytesThisWindow);
        m_windowStart = now;
        m_sentBytesThisWindow = 0;
    } else {
        Logger::Trace("[ClientConnection::ResetWindowIfNeeded] Client %u: window still active (elapsed=%lld s), no reset",
                      m_clientId, (long long)elapsed);
    }
    Logger::Trace("[ClientConnection::ResetWindowIfNeeded] Exit");
}

int ClientConnection::GetPing() const {
    return m_pingMs;
}

void ClientConnection::UpdatePing(int pingMs) {
    m_pingMs = pingMs;
}
