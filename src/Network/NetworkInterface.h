// src/Network/NetworkInterface.h – Header for NetworkInterface

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "Network/Packet.h"

struct ClientAddress {
    std::string ip;
    uint16_t    port;
};

class NetworkInterface {
public:
    using PacketHandler = std::function<void(uint32_t clientId, const Packet& pkt)>;

    NetworkInterface();
    ~NetworkInterface();

    // Initialize underlying socket on given port
    bool Initialize(uint16_t listenPort);

    // Register callback for incoming packets
    void SetPacketCallback(PacketHandler handler);

    // Send raw data to a client
    bool SendTo(const ClientAddress& addr, const std::vector<uint8_t>& data);

    // Send a structured Packet
    bool SendPacket(const ClientAddress& addr, const Packet& pkt);

    // Receive and dispatch packets (called each tick)
    void Poll();

    // Helpers
    void Close();
    bool IsOpen() const;

private:
    int              m_socketFd;
    uint16_t         m_listenPort;
    PacketHandler    m_handler;

    // Internal receive buffer
    static constexpr size_t MAX_PACKET_SIZE = 1500;
    std::vector<uint8_t>    m_recvBuffer;

    // System calls wrapped
    bool BindSocket(uint16_t port);
    int  RecvFrom(std::string& outIp, uint16_t& outPort);
    bool SendToInternal(const std::string& ip, uint16_t port, const uint8_t* data, size_t len);
};