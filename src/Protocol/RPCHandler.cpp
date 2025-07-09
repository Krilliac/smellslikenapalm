// src/Protocol/RPCHandler.cpp
#include "Protocol/RPCHandler.h"
#include "Utils/Logger.h"
#include "Protocol/ProtocolUtils.h"

RPCHandler::RPCHandler() = default;
RPCHandler::~RPCHandler() = default;

void RPCHandler::RegisterRPC(const std::string& name, RPCFunc func) {
    m_rpcMap[name] = std::move(func);
    Logger::Info("RPCHandler: registered RPC '%s'", name.c_str());
}

void RPCHandler::Handle(uint32_t clientId,
                        const Packet& pkt,
                        const PacketMetadata& meta,
                        std::function<void(const Packet&)> sendFunc)
{
    const std::string& tag = pkt.GetTag();
    if (tag == m_requestTag) {
        // Decode RPC call name and args
        Packet reader = pkt;
        std::string rpcName = reader.ReadString();
        uint32_t argLen    = reader.ReadUInt();
        std::vector<uint8_t> args = reader.ReadBytes(argLen);

        auto it = m_rpcMap.find(rpcName);
        if (it != m_rpcMap.end()) {
            auto maybeReply = it->second(clientId, pkt, meta);
            if (maybeReply) {
                sendFunc(*maybeReply);
            }
        } else {
            Logger::Warn("RPCHandler: unknown RPC '%s' called by %u", rpcName.c_str(), clientId);
        }
    }
    else if (tag == m_responseTag) {
        // Responses may be handled by client‐side code; here just log or forward
        Packet reader = pkt;
        std::string rpcName = reader.ReadString();
        uint32_t resLen    = reader.ReadUInt();
        std::vector<uint8_t> result = reader.ReadBytes(resLen);
        Logger::Debug("RPCHandler: received response to '%s' from %u (%u bytes)",
                      rpcName.c_str(), clientId, resLen);
        // Client‐side should subscribe to a response dispatcher
    }
    else {
        // Not an RPC packet
    }
}

Packet RPCHandler::BuildRequest(const std::string& rpcName, const std::vector<uint8_t>& args) {
    Packet pkt(m_requestTag);
    pkt.WriteString(rpcName);
    pkt.WriteUInt(static_cast<uint32_t>(args.size()));
    pkt.WriteBytes(args);
    return pkt;
}

Packet RPCHandler::BuildResponse(const std::string& rpcName, const std::vector<uint8_t>& result) {
    Packet pkt(m_responseTag);
    pkt.WriteString(rpcName);
    pkt.WriteUInt(static_cast<uint32_t>(result.size()));
    pkt.WriteBytes(result);
    return pkt;
}