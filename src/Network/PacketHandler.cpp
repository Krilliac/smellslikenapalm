// src/Network/PacketHandler.cpp

#include "Network/PacketHandler.h"
#include "Utils/Logger.h"
#include "Game/GameServer.h"

PacketHandler::PacketHandler(NetworkManager* netMgr)
    : m_network(netMgr)
{
    Logger::Trace("[PacketHandler::PacketHandler] Entry: netMgr=%p", (void*)netMgr);
    Logger::Info("PacketHandler initialized");
    Logger::Debug("[PacketHandler::PacketHandler] m_network=%p", (void*)m_network);
    Logger::Trace("[PacketHandler::PacketHandler] Exit");
}

PacketHandler::~PacketHandler() {
    Logger::Trace("[PacketHandler::~PacketHandler] Entry: destructor called");
    Logger::Debug("[PacketHandler::~PacketHandler] Destroying PacketHandler, %zu handlers registered",
                  m_handlers.size());
    Logger::Trace("[PacketHandler::~PacketHandler] Exit");
}

void PacketHandler::RegisterHandler(const std::string& tag, HandlerFunc handler) {
    Logger::Trace("[PacketHandler::RegisterHandler] Entry: tag='%s', handler=%s",
                  tag.c_str(), handler ? "non-null" : "null");
    m_handlers[tag] = std::move(handler);
    Logger::Debug("PacketHandler: Registered handler for tag '%s'", tag.c_str());
    Logger::Debug("[PacketHandler::RegisterHandler] Total registered handlers: %zu", m_handlers.size());
    Logger::Trace("[PacketHandler::RegisterHandler] Exit");
}

void PacketHandler::SetDefaultHandler(HandlerFunc handler) {
    Logger::Trace("[PacketHandler::SetDefaultHandler] Entry: handler=%s",
                  handler ? "non-null" : "null");
    m_defaultHandler = std::move(handler);
    Logger::Debug("PacketHandler: Default handler set");
    Logger::Trace("[PacketHandler::SetDefaultHandler] Exit");
}

void PacketHandler::HandlePacket(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    Logger::Trace("[PacketHandler::HandlePacket] Entry: clientId=%u, tag='%s', payloadSize=%u",
                  clientId, pkt.GetTag().c_str(), pkt.GetPayloadSize());
    const std::string& tag = pkt.GetTag();
    auto it = m_handlers.find(tag);
    if (it != m_handlers.end()) {
        Logger::Debug("PacketHandler: Dispatching '%s' from client %u", tag.c_str(), clientId);
        Logger::Debug("[PacketHandler::HandlePacket] Found registered handler for tag '%s', invoking", tag.c_str());
        it->second(clientId, pkt, meta);
        Logger::Debug("[PacketHandler::HandlePacket] Handler for '%s' completed successfully", tag.c_str());
    } else if (m_defaultHandler) {
        Logger::Warn("PacketHandler: No handler for '%s', using default", tag.c_str());
        Logger::Debug("[PacketHandler::HandlePacket] Falling back to default handler for tag '%s' from client %u",
                      tag.c_str(), clientId);
        m_defaultHandler(clientId, pkt, meta);
        Logger::Debug("[PacketHandler::HandlePacket] Default handler completed for tag '%s'", tag.c_str());
    } else {
        Logger::Warn("PacketHandler: Dropping unhandled packet '%s'", tag.c_str());
        Logger::Debug("[PacketHandler::HandlePacket] No handler and no default handler, packet dropped: tag='%s', clientId=%u",
                      tag.c_str(), clientId);
    }
    Logger::Trace("[PacketHandler::HandlePacket] Exit");
}
