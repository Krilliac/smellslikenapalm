// src/Network/PacketHandler.cpp

#include "Network/PacketHandler.h"
#include "Utils/Logger.h"
#include "Game/GameServer.h"

PacketHandler::PacketHandler(NetworkManager* netMgr)
    : m_network(netMgr)
{
    Logger::Info("PacketHandler initialized");
}

PacketHandler::~PacketHandler() = default;

void PacketHandler::RegisterHandler(const std::string& tag, HandlerFunc handler) {
    m_handlers[tag] = std::move(handler);
    Logger::Debug("PacketHandler: Registered handler for tag '%s'", tag.c_str());
}

void PacketHandler::SetDefaultHandler(HandlerFunc handler) {
    m_defaultHandler = std::move(handler);
    Logger::Debug("PacketHandler: Default handler set");
}

void PacketHandler::HandlePacket(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    const std::string& tag = pkt.GetTag();
    auto it = m_handlers.find(tag);
    if (it != m_handlers.end()) {
        Logger::Debug("PacketHandler: Dispatching '%s' from client %u", tag.c_str(), clientId);
        it->second(clientId, pkt, meta);
    } else if (m_defaultHandler) {
        Logger::Warn("PacketHandler: No handler for '%s', using default", tag.c_str());
        m_defaultHandler(clientId, pkt, meta);
    } else {
        Logger::Warn("PacketHandler: Dropping unhandled packet '%s'", tag.c_str());
    }
}