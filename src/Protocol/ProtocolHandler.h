// src/Protocol/ProtocolHandler.h
#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/MessageDecoder.h"
#include "Protocol/MessageEncoder.h"

struct PacketMetadata;

class ProtocolHandler {
public:
    using HandlerFunc = std::function<void(uint32_t clientId, const Packet&, const PacketMetadata&)>;

    explicit ProtocolHandler();
    ~ProtocolHandler();

    // Register handler for a specific PacketType
    void RegisterHandler(PacketType type, HandlerFunc handler);

    // Register fallback for unhandled types
    void SetDefaultHandler(HandlerFunc handler);

    // Dispatch incoming packet by tag→enum→handler
    void Handle(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);

private:
    // Map from PacketType to handler function
    std::unordered_map<PacketType, HandlerFunc> m_handlers;
    HandlerFunc                                 m_defaultHandler;

    // Convert tag string to PacketType enum
    PacketType TagToType(const std::string& tag) const;
};