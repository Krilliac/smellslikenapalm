// src/Protocol/ProtocolHandler.cpp
#include "Protocol/ProtocolHandler.h"
#include "Utils/Logger.h"

ProtocolHandler::ProtocolHandler() {
    Logger::Trace("[ProtocolHandler::ProtocolHandler] entry — constructing default ProtocolHandler instance");
    Logger::Trace("[ProtocolHandler::ProtocolHandler] exit — default construction complete");
}

ProtocolHandler::~ProtocolHandler() {
    Logger::Trace("[ProtocolHandler::~ProtocolHandler] entry — destroying ProtocolHandler instance");
    Logger::Debug("[ProtocolHandler::~ProtocolHandler] handler count at destruction: %zu registered handlers, defaultHandler=%s",
                  m_handlers.size(), m_defaultHandler ? "set" : "null");
    Logger::Trace("[ProtocolHandler::~ProtocolHandler] exit — destruction complete");
}

void ProtocolHandler::RegisterHandler(PacketType type, HandlerFunc handler) {
    Logger::Trace("[ProtocolHandler::RegisterHandler] entry — type=%s (%d), handler=%s",
                  ToString(type), static_cast<int>(type), handler ? "valid" : "null");
    m_handlers[type] = std::move(handler);
    Logger::Debug("ProtocolHandler: registered handler for %s", ToString(type));
    Logger::Info("[ProtocolHandler::RegisterHandler] handler registered for packet type '%s' — total registered handlers: %zu",
                 ToString(type), m_handlers.size());
    Logger::Trace("[ProtocolHandler::RegisterHandler] exit — handler registration complete for type=%s", ToString(type));
}

void ProtocolHandler::SetDefaultHandler(HandlerFunc handler) {
    Logger::Trace("[ProtocolHandler::SetDefaultHandler] entry — handler=%s", handler ? "valid" : "null");
    m_defaultHandler = std::move(handler);
    Logger::Debug("ProtocolHandler: default handler set");
    Logger::Info("[ProtocolHandler::SetDefaultHandler] default fallback handler has been configured");
    Logger::Trace("[ProtocolHandler::SetDefaultHandler] exit — default handler set successfully");
}

PacketType ProtocolHandler::TagToType(const std::string& tag) const {
    Logger::Trace("[ProtocolHandler::TagToType] entry — tag='%s'", tag.c_str());
    PacketType result = FromString(tag);
    Logger::Debug("[ProtocolHandler::TagToType] resolved tag='%s' to PacketType=%s (%d)",
                  tag.c_str(), ToString(result), static_cast<int>(result));
    Logger::Trace("[ProtocolHandler::TagToType] exit — returning PacketType=%s", ToString(result));
    return result;
}

void ProtocolHandler::Handle(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    Logger::Trace("[ProtocolHandler::Handle] entry — clientId=%u, packet tag='%s'",
                  clientId, pkt.GetTag().c_str());
    PacketType type = TagToType(pkt.GetTag());
    Logger::Debug("[ProtocolHandler::Handle] resolved packet tag='%s' to type=%s (%d) for clientId=%u",
                  pkt.GetTag().c_str(), ToString(type), static_cast<int>(type), clientId);
    auto it = m_handlers.find(type);
    if (it != m_handlers.end()) {
        Logger::Debug("[ProtocolHandler::Handle] found registered handler for type=%s — dispatching for clientId=%u",
                      ToString(type), clientId);
        it->second(clientId, pkt, meta);
        Logger::Info("[ProtocolHandler::Handle] dispatched packet type=%s from clientId=%u to registered handler",
                     ToString(type), clientId);
    } else if (m_defaultHandler) {
        Logger::Debug("[ProtocolHandler::Handle] no specific handler for type=%s — falling back to default handler for clientId=%u",
                      ToString(type), clientId);
        m_defaultHandler(clientId, pkt, meta);
        Logger::Info("[ProtocolHandler::Handle] dispatched packet type=%s from clientId=%u to default handler",
                     ToString(type), clientId);
    } else {
        Logger::Warn("ProtocolHandler: unhandled packet tag '%s'", pkt.GetTag().c_str());
        Logger::Debug("[ProtocolHandler::Handle] no handler registered for type=%s and no default handler set — packet from clientId=%u dropped",
                      ToString(type), clientId);
    }
    Logger::Trace("[ProtocolHandler::Handle] exit — finished handling packet tag='%s' for clientId=%u",
                  pkt.GetTag().c_str(), clientId);
}
