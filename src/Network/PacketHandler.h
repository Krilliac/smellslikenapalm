// src/Network/PacketHandler.h

#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include "Network/Packet.h"

class NetworkManager;

class PacketHandler {
public:
    using HandlerFunc = std::function<void(uint32_t clientId, const Packet&, const PacketMetadata&)>;

    explicit PacketHandler(NetworkManager* netMgr);
    ~PacketHandler();

    // Register a handler for a specific packet tag
    void RegisterHandler(const std::string& tag, HandlerFunc handler);

    // Dispatch incoming packet to the appropriate handler
    void HandlePacket(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);

    // Default fallback for unhandled packets
    void SetDefaultHandler(HandlerFunc handler);

private:
    NetworkManager* m_network;
    std::unordered_map<std::string, HandlerFunc> m_handlers;
    HandlerFunc m_defaultHandler;
};