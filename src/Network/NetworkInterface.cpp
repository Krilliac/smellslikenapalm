// src/Network/NetworkInterface.cpp – Implementation for NetworkInterface

#include "Network/NetworkInterface.h"
#include "Utils/Logger.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

NetworkInterface::NetworkInterface()
    : m_socketFd(-1), m_listenPort(0), m_recvBuffer(MAX_PACKET_SIZE)
{
    Logger::Trace("[NetworkInterface::NetworkInterface] Entry: default constructor");
    Logger::Debug("[NetworkInterface::NetworkInterface] Initialized with socketFd=-1, listenPort=0, recvBuffer size=%zu", (size_t)MAX_PACKET_SIZE);
    Logger::Trace("[NetworkInterface::NetworkInterface] Exit");
}

NetworkInterface::~NetworkInterface() {
    Logger::Trace("[NetworkInterface::~NetworkInterface] Entry: destructor called");
    Logger::Debug("[NetworkInterface::~NetworkInterface] Destroying NetworkInterface, socketFd=%d, listenPort=%u", m_socketFd, m_listenPort);
    Close();
    Logger::Trace("[NetworkInterface::~NetworkInterface] Exit");
}

bool NetworkInterface::Initialize(uint16_t listenPort) {
    Logger::Trace("[NetworkInterface::Initialize] Entry: listenPort=%u", listenPort);
    m_listenPort = listenPort;
    Logger::Debug("[NetworkInterface::Initialize] Set m_listenPort=%u, attempting to bind socket", listenPort);
    if (!BindSocket(listenPort)) {
        Logger::Error("NetworkInterface: Failed to bind on port %u", listenPort);
        Logger::Trace("[NetworkInterface::Initialize] Exit: returning false (bind failed)");
        return false;
    }
    Logger::Info("NetworkInterface: Listening on UDP port %u", listenPort);
    Logger::Debug("[NetworkInterface::Initialize] Socket bound successfully, socketFd=%d", m_socketFd);
    Logger::Trace("[NetworkInterface::Initialize] Exit: returning true");
    return true;
}

void NetworkInterface::SetPacketCallback(PacketHandler handler) {
    Logger::Trace("[NetworkInterface::SetPacketCallback] Entry: handler=%s", handler ? "non-null" : "null");
    m_handler = std::move(handler);
    Logger::Debug("[NetworkInterface::SetPacketCallback] Packet callback handler set successfully");
    Logger::Trace("[NetworkInterface::SetPacketCallback] Exit");
}

bool NetworkInterface::SendTo(const ClientAddress& addr, const std::vector<uint8_t>& data) {
    Logger::Trace("[NetworkInterface::SendTo] Entry: addr=%s:%u, data size=%zu",
                  addr.ip.c_str(), addr.port, data.size());
    bool result = SendToInternal(addr.ip, addr.port, data.data(), data.size());
    Logger::Debug("[NetworkInterface::SendTo] SendToInternal returned %s for %s:%u (%zu bytes)",
                  result ? "true" : "false", addr.ip.c_str(), addr.port, data.size());
    Logger::Trace("[NetworkInterface::SendTo] Exit: returning %s", result ? "true" : "false");
    return result;
}

bool NetworkInterface::SendPacket(const ClientAddress& addr, const Packet& pkt) {
    Logger::Trace("[NetworkInterface::SendPacket] Entry: addr=%s:%u, packet tag='%s'",
                  addr.ip.c_str(), addr.port, pkt.GetTag().c_str());
    auto buf = pkt.Serialize();
    Logger::Debug("[NetworkInterface::SendPacket] Serialized packet: tag='%s', serialized size=%zu",
                  pkt.GetTag().c_str(), buf.size());
    bool result = SendToInternal(addr.ip, addr.port, buf.data(), buf.size());
    Logger::Debug("[NetworkInterface::SendPacket] SendToInternal returned %s", result ? "true" : "false");
    Logger::Trace("[NetworkInterface::SendPacket] Exit: returning %s", result ? "true" : "false");
    return result;
}

void NetworkInterface::Poll() {
    Logger::Trace("[NetworkInterface::Poll] Entry");
    if (m_socketFd < 0 || !m_handler) {
        Logger::Debug("[NetworkInterface::Poll] Early return: socketFd=%d, handler=%s",
                      m_socketFd, m_handler ? "set" : "not set");
        Logger::Trace("[NetworkInterface::Poll] Exit: preconditions not met");
        return;
    }

    std::string srcIp;
    uint16_t srcPort;
    Logger::Debug("[NetworkInterface::Poll] Calling RecvFrom on socketFd=%d", m_socketFd);
    int len = RecvFrom(srcIp, srcPort);
    if (len <= 0) {
        Logger::Debug("[NetworkInterface::Poll] RecvFrom returned %d, no data available", len);
        Logger::Trace("[NetworkInterface::Poll] Exit: no data received");
        return;
    }

    Logger::Debug("[NetworkInterface::Poll] Received %d bytes from %s:%u", len, srcIp.c_str(), srcPort);
    PacketMetadata meta;
    Packet pkt = Packet::FromBuffer(m_recvBuffer, meta);
    uint32_t clientId = meta.clientId;  // Assuming PacketMetadata carries client mapping
    Logger::Debug("[NetworkInterface::Poll] Parsed packet: tag='%s', clientId=%u, payloadSize=%u",
                  pkt.GetTag().c_str(), clientId, pkt.GetPayloadSize());

    Logger::Info("[NetworkInterface::Poll] Dispatching packet '%s' from client %u (%s:%u)",
                 pkt.GetTag().c_str(), clientId, srcIp.c_str(), srcPort);
    m_handler(clientId, pkt);
    Logger::Trace("[NetworkInterface::Poll] Exit: packet dispatched");
}

void NetworkInterface::Close() {
    Logger::Trace("[NetworkInterface::Close] Entry: socketFd=%d", m_socketFd);
    if (m_socketFd >= 0) {
        Logger::Debug("[NetworkInterface::Close] Closing socket fd=%d on port %u", m_socketFd, m_listenPort);
        close(m_socketFd);
        m_socketFd = -1;
        Logger::Info("NetworkInterface: Socket closed");
    } else {
        Logger::Debug("[NetworkInterface::Close] Socket already closed (fd=%d), nothing to do", m_socketFd);
    }
    Logger::Trace("[NetworkInterface::Close] Exit");
}

bool NetworkInterface::IsOpen() const {
    Logger::Trace("[NetworkInterface::IsOpen] Entry");
    bool open = m_socketFd >= 0;
    Logger::Debug("[NetworkInterface::IsOpen] socketFd=%d, returning %s", m_socketFd, open ? "true" : "false");
    Logger::Trace("[NetworkInterface::IsOpen] Exit: returning %s", open ? "true" : "false");
    return open;
}

bool NetworkInterface::BindSocket(uint16_t port) {
    Logger::Trace("[NetworkInterface::BindSocket] Entry: port=%u", port);
    m_socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socketFd < 0) {
        Logger::Error("[NetworkInterface::BindSocket] socket() creation failed for port %u", port);
        Logger::Trace("[NetworkInterface::BindSocket] Exit: returning false (socket creation failed)");
        return false;
    }
    Logger::Debug("[NetworkInterface::BindSocket] Created UDP socket fd=%d", m_socketFd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(m_socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    Logger::Debug("[NetworkInterface::BindSocket] Set SO_REUSEADDR on fd=%d", m_socketFd);

    if (bind(m_socketFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("[NetworkInterface::BindSocket] bind() failed on port %u, fd=%d", port, m_socketFd);
        close(m_socketFd);
        m_socketFd = -1;
        Logger::Debug("[NetworkInterface::BindSocket] Socket closed and reset to -1 after bind failure");
        Logger::Trace("[NetworkInterface::BindSocket] Exit: returning false (bind failed)");
        return false;
    }
    Logger::Debug("[NetworkInterface::BindSocket] Successfully bound to INADDR_ANY:%u on fd=%d", port, m_socketFd);
    Logger::Trace("[NetworkInterface::BindSocket] Exit: returning true");
    return true;
}

int NetworkInterface::RecvFrom(std::string& outIp, uint16_t& outPort) {
    Logger::Trace("[NetworkInterface::RecvFrom] Entry: socketFd=%d", m_socketFd);
    sockaddr_in srcAddr{};
    socklen_t addrLen = sizeof(srcAddr);
    int len = recvfrom(m_socketFd, m_recvBuffer.data(), MAX_PACKET_SIZE, 0,
                       (sockaddr*)&srcAddr, &addrLen);
    if (len > 0) {
        outIp = inet_ntoa(srcAddr.sin_addr);
        outPort = ntohs(srcAddr.sin_port);
        Logger::Debug("[NetworkInterface::RecvFrom] Received %d bytes from %s:%u", len, outIp.c_str(), outPort);
    } else {
        Logger::Debug("[NetworkInterface::RecvFrom] recvfrom returned %d (no data or error)", len);
    }
    Logger::Trace("[NetworkInterface::RecvFrom] Exit: returning %d", len);
    return len;
}

bool NetworkInterface::SendToInternal(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    Logger::Trace("[NetworkInterface::SendToInternal] Entry: ip=%s, port=%u, data=%p, len=%zu",
                  ip.c_str(), port, (const void*)data, len);
    if (m_socketFd < 0) {
        Logger::Error("[NetworkInterface::SendToInternal] Cannot send: socket not open (fd=%d)", m_socketFd);
        Logger::Trace("[NetworkInterface::SendToInternal] Exit: returning false (socket not open)");
        return false;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(ip.c_str());
    dest.sin_port = htons(port);

    Logger::Debug("[NetworkInterface::SendToInternal] Sending %zu bytes to %s:%u via fd=%d", len, ip.c_str(), port, m_socketFd);
    ssize_t sent = sendto(m_socketFd, data, len, 0, (sockaddr*)&dest, sizeof(dest));
    if (sent != (ssize_t)len) {
        Logger::Warn("NetworkInterface: Partial/failed send to %s:%u", ip.c_str(), port);
        Logger::Error("[NetworkInterface::SendToInternal] sendto returned %zd, expected %zu for %s:%u",
                      sent, len, ip.c_str(), port);
        Logger::Trace("[NetworkInterface::SendToInternal] Exit: returning false (partial/failed send)");
        return false;
    }
    Logger::Debug("[NetworkInterface::SendToInternal] Successfully sent %zd bytes to %s:%u", sent, ip.c_str(), port);
    Logger::Trace("[NetworkInterface::SendToInternal] Exit: returning true");
    return true;
}
