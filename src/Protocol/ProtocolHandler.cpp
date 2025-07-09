// src/Protocol/ProtocolHandler.cpp
#include "Protocol/ProtocolHandler.h"
#include "Utils/Logger.h"

ProtocolHandler::ProtocolHandler() = default;

ProtocolHandler::~ProtocolHandler() = default;

void ProtocolHandler::RegisterHandler(PacketType type, HandlerFunc handler) {
    m_handlers[type] = std::move(handler);
    Logger::Debug("ProtocolHandler: registered handler for %s", ToString(type));
}

void ProtocolHandler::SetDefaultHandler(HandlerFunc handler) {
    m_defaultHandler = std::move(handler);
    Logger::Debug("ProtocolHandler: default handler set");
}

PacketType ProtocolHandler::TagToType(const std::string& tag) const {
    return FromString(tag);
}

void ProtocolHandler::Handle(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    PacketType type = TagToType(pkt.GetTag());
    auto it = m_handlers.find(type);
    if (it != m_handlers.end()) {
        it->second(clientId, pkt, meta);
    } else if (m_defaultHandler) {
        m_defaultHandler(clientId, pkt, meta);
    } else {
        Logger::Warn("ProtocolHandler: unhandled packet tag '%s'", pkt.GetTag().c_str());
    }
}