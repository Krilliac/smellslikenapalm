// src/Network/ClientConnection.cpp

#include "Network/ClientConnection.h"
#include "Network/ConnectionManager.h"
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
    Logger::Info("ClientConnection created: ID=%u %s:%u", clientId, ip.c_str(), port);
}

ClientConnection::~ClientConnection() = default;

uint32_t ClientConnection::GetClientId() const { return m_clientId; }
const std::string& ClientConnection::GetIP() const { return m_ip; }
uint16_t ClientConnection::GetPort() const { return m_port; }

std::string ClientConnection::GetSteamID() const {
    return std::to_string(m_clientId);
}

bool ClientConnection::SendPacket(const Packet& pkt) {
    auto data = pkt.Serialize();
    if (!CanSend((uint32_t)data.size())) return false;
    bool ok = m_socket->SendTo(m_ip, m_port, data.data(), data.size());
    if (ok) {
        OnBytesSent((uint32_t)data.size());
        // Feed outbound packet to protocol decoder
        GetProtocolDecoder().OnPacketSent(m_clientId, pkt.RawData(), pkt.GetTag());
    }
    return ok;
}

bool ClientConnection::ReceiveRaw(std::vector<uint8_t>& outData, PacketMetadata& meta) {
    outData.resize(1500);
    std::string fromIp;
    uint16_t fromPort = 0;
    int len = m_socket->ReceiveFrom(fromIp, fromPort, outData.data(), (int)outData.size());
    if (len <= 0) return false;
    outData.resize(len);
    m_lastRaw = outData;
    m_lastPacket = Packet::FromBuffer(outData, meta);
    return true;
}

Packet ClientConnection::LastPacket() const { return m_lastPacket; }
std::vector<uint8_t> ClientConnection::LastRawPacket() const { return m_lastRaw; }

void ClientConnection::MarkDisconnected() { m_disconnected = true; }
bool ClientConnection::IsDisconnected() const { return m_disconnected; }

void ClientConnection::UpdateLastHeartbeat() {
    m_lastHeartbeat = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point ClientConnection::GetLastHeartbeat() const {
    return m_lastHeartbeat;
}

// Player name/team management

void ClientConnection::SetPlayerName(const std::string& name) { m_playerName = name; }
const std::string& ClientConnection::GetPlayerName() const { return m_playerName; }
uint32_t ClientConnection::GetTeamId() const { return m_teamId; }
void ClientConnection::SetTeamId(uint32_t teamId) { m_teamId = teamId; }

// Game-layer helpers

void ClientConnection::SendInventoryUpdate(const std::vector<InventoryItem>& items) {
    Packet p("INVENTORY_UPDATE");
    p.WriteUInt(static_cast<uint32_t>(items.size()));
    for (const auto& item : items) {
        p.WriteString(item.name);
        p.WriteInt(item.quantity);
    }
    SendPacket(p);
}

void ClientConnection::SendChatMessage(const std::string& msg) {
    Packet p("CHAT_MESSAGE");
    p.WriteString(msg);
    SendPacket(p);
}

void ClientConnection::SendPositionUpdate(const Vector3& pos) {
    Packet p("PLAYER_POSITION");
    p.WriteVector3(pos);
    SendPacket(p);
}

void ClientConnection::SendOrientationUpdate(const Vector3& dir) {
    Packet p("PLAYER_ORIENTATION");
    p.WriteVector3(dir);
    SendPacket(p);
}

void ClientConnection::SendHealthUpdate(int hp) {
    Packet p("HEALTH_UPDATE");
    p.WriteInt(hp);
    SendPacket(p);
}

void ClientConnection::SendTeamUpdate(uint32_t teamId) {
    Packet p("TEAM_UPDATE");
    p.WriteUInt(teamId);
    SendPacket(p);
}

void ClientConnection::SendSpawnPlayer() {
    Packet p("SPAWN_PLAYER");
    SendPacket(p);
}

void ClientConnection::SendSessionState(uint32_t aliveCount) {
    Packet p("SESSION_STATE");
    p.WriteUInt(aliveCount);
    SendPacket(p);
}

// Bandwidth / rate limiting

bool ClientConnection::CanSend(uint32_t byteCount) {
    ResetWindowIfNeeded();
    return m_sentBytesThisWindow + byteCount <= m_manager->GetBandwidthLimit();
}

void ClientConnection::OnBytesSent(uint32_t byteCount) {
    ResetWindowIfNeeded();
    m_sentBytesThisWindow += byteCount;
}

void ClientConnection::ResetWindowIfNeeded() {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - m_windowStart).count() >= 1) {
        m_windowStart = now;
        m_sentBytesThisWindow = 0;
    }
}
