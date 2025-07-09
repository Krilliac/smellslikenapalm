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
{}

NetworkInterface::~NetworkInterface() {
    Close();
}

bool NetworkInterface::Initialize(uint16_t listenPort) {
    m_listenPort = listenPort;
    if (!BindSocket(listenPort)) {
        Logger::Error("NetworkInterface: Failed to bind on port %u", listenPort);
        return false;
    }
    Logger::Info("NetworkInterface: Listening on UDP port %u", listenPort);
    return true;
}

void NetworkInterface::SetPacketCallback(PacketHandler handler) {
    m_handler = std::move(handler);
}

bool NetworkInterface::SendTo(const ClientAddress& addr, const std::vector<uint8_t>& data) {
    return SendToInternal(addr.ip, addr.port, data.data(), data.size());
}

bool NetworkInterface::SendPacket(const ClientAddress& addr, const Packet& pkt) {
    auto buf = pkt.Serialize();
    return SendToInternal(addr.ip, addr.port, buf.data(), buf.size());
}

void NetworkInterface::Poll() {
    if (m_socketFd < 0 || !m_handler) return;

    std::string srcIp;
    uint16_t srcPort;
    int len = RecvFrom(srcIp, srcPort);
    if (len <= 0) return;

    PacketMetadata meta;
    Packet pkt = Packet::FromBuffer(m_recvBuffer, meta);
    uint32_t clientId = meta.clientId;  // Assuming PacketMetadata carries client mapping

    m_handler(clientId, pkt);
}

void NetworkInterface::Close() {
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
        Logger::Info("NetworkInterface: Socket closed");
    }
}

bool NetworkInterface::IsOpen() const {
    return m_socketFd >= 0;
}

bool NetworkInterface::BindSocket(uint16_t port) {
    m_socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socketFd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(m_socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(m_socketFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    return true;
}

int NetworkInterface::RecvFrom(std::string& outIp, uint16_t& outPort) {
    sockaddr_in srcAddr{};
    socklen_t addrLen = sizeof(srcAddr);
    int len = recvfrom(m_socketFd, m_recvBuffer.data(), MAX_PACKET_SIZE, 0,
                       (sockaddr*)&srcAddr, &addrLen);
    if (len > 0) {
        outIp = inet_ntoa(srcAddr.sin_addr);
        outPort = ntohs(srcAddr.sin_port);
    }
    return len;
}

bool NetworkInterface::SendToInternal(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    if (m_socketFd < 0) return false;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(ip.c_str());
    dest.sin_port = htons(port);

    ssize_t sent = sendto(m_socketFd, data, len, 0, (sockaddr*)&dest, sizeof(dest));
    if (sent != (ssize_t)len) {
        Logger::Warn("NetworkInterface: Partial/failed send to %s:%u", ip.c_str(), port);
        return false;
    }
    return true;
}