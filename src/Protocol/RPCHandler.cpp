// src/Protocol/RPCHandler.cpp
#include "Protocol/RPCHandler.h"
#include "Utils/Logger.h"
#include "Protocol/ProtocolUtils.h"

RPCHandler::RPCHandler() {
    Logger::Trace("[RPCHandler::RPCHandler] entry — constructing default RPCHandler");
    Logger::Debug("[RPCHandler::RPCHandler] initialized with empty RPC map, requestTag='%s', responseTag='%s'",
                  m_requestTag.c_str(), m_responseTag.c_str());
    Logger::Trace("[RPCHandler::RPCHandler] exit — construction complete");
}

RPCHandler::~RPCHandler() {
    Logger::Trace("[RPCHandler::~RPCHandler] entry — destroying RPCHandler");
    Logger::Debug("[RPCHandler::~RPCHandler] %zu registered RPCs at destruction time", m_rpcMap.size());
    Logger::Trace("[RPCHandler::~RPCHandler] exit — destruction complete");
}

void RPCHandler::RegisterRPC(const std::string& name, RPCFunc func) {
    Logger::Trace("[RPCHandler::RegisterRPC] entry — name='%s', func=%s",
                  name.c_str(), func ? "valid" : "null");
    m_rpcMap[name] = std::move(func);
    Logger::Info("RPCHandler: registered RPC '%s'", name.c_str());
    Logger::Debug("[RPCHandler::RegisterRPC] total registered RPCs: %zu", m_rpcMap.size());
    Logger::Trace("[RPCHandler::RegisterRPC] exit — RPC '%s' registered successfully", name.c_str());
}

void RPCHandler::Handle(uint32_t clientId,
                        const Packet& pkt,
                        const PacketMetadata& meta,
                        std::function<void(const Packet&)> sendFunc)
{
    Logger::Trace("[RPCHandler::Handle] entry — clientId=%u, packet tag='%s', sendFunc=%s",
                  clientId, pkt.GetTag().c_str(), sendFunc ? "valid" : "null");
    const std::string& tag = pkt.GetTag();
    if (tag == m_requestTag) {
        Logger::Debug("[RPCHandler::Handle] packet tag '%s' matches requestTag — processing RPC request from clientId=%u",
                      tag.c_str(), clientId);
        // Decode RPC call name and args
        Packet reader = pkt;
        std::string rpcName = reader.ReadString();
        uint32_t argLen    = reader.ReadUInt();
        std::vector<uint8_t> args = reader.ReadBytes(argLen);
        Logger::Debug("[RPCHandler::Handle] decoded RPC request: rpcName='%s', argLen=%u bytes from clientId=%u",
                      rpcName.c_str(), argLen, clientId);

        auto it = m_rpcMap.find(rpcName);
        if (it != m_rpcMap.end()) {
            Logger::Debug("[RPCHandler::Handle] found registered handler for RPC '%s' — invoking handler", rpcName.c_str());
            auto maybeReply = it->second(clientId, pkt, meta);
            if (maybeReply) {
                Logger::Debug("[RPCHandler::Handle] RPC '%s' handler returned a reply packet — sending response to clientId=%u",
                              rpcName.c_str(), clientId);
                sendFunc(*maybeReply);
                Logger::Info("[RPCHandler::Handle] RPC '%s' from clientId=%u handled and response sent",
                             rpcName.c_str(), clientId);
            } else {
                Logger::Debug("[RPCHandler::Handle] RPC '%s' handler returned no reply — no response to send", rpcName.c_str());
                Logger::Info("[RPCHandler::Handle] RPC '%s' from clientId=%u handled (no response generated)",
                             rpcName.c_str(), clientId);
            }
        } else {
            Logger::Warn("RPCHandler: unknown RPC '%s' called by %u", rpcName.c_str(), clientId);
            Logger::Debug("[RPCHandler::Handle] RPC '%s' not found in map of %zu registered RPCs",
                          rpcName.c_str(), m_rpcMap.size());
        }
    }
    else if (tag == m_responseTag) {
        Logger::Debug("[RPCHandler::Handle] packet tag '%s' matches responseTag — processing RPC response from clientId=%u",
                      tag.c_str(), clientId);
        // Responses may be handled by client-side code; here just log or forward
        Packet reader = pkt;
        std::string rpcName = reader.ReadString();
        uint32_t resLen    = reader.ReadUInt();
        std::vector<uint8_t> result = reader.ReadBytes(resLen);
        Logger::Debug("RPCHandler: received response to '%s' from %u (%u bytes)",
                      rpcName.c_str(), clientId, resLen);
        Logger::Info("[RPCHandler::Handle] received RPC response for '%s' from clientId=%u with %u bytes of result data",
                     rpcName.c_str(), clientId, resLen);
        // Client-side should subscribe to a response dispatcher
    }
    else {
        Logger::Debug("[RPCHandler::Handle] packet tag '%s' does not match requestTag='%s' or responseTag='%s' — not an RPC packet, ignoring",
                      tag.c_str(), m_requestTag.c_str(), m_responseTag.c_str());
        // Not an RPC packet
    }
    Logger::Trace("[RPCHandler::Handle] exit — finished handling packet tag='%s' from clientId=%u",
                  tag.c_str(), clientId);
}

Packet RPCHandler::BuildRequest(const std::string& rpcName, const std::vector<uint8_t>& args) {
    Logger::Trace("[RPCHandler::BuildRequest] entry — rpcName='%s', args.size()=%zu bytes",
                  rpcName.c_str(), args.size());
    Packet pkt("RPC_CALL");
    pkt.WriteString(rpcName);
    pkt.WriteUInt(static_cast<uint32_t>(args.size()));
    pkt.WriteBytes(args);
    Logger::Debug("[RPCHandler::BuildRequest] built RPC request packet: rpcName='%s', argLen=%zu bytes, tag='RPC_CALL'",
                  rpcName.c_str(), args.size());
    Logger::Info("[RPCHandler::BuildRequest] built RPC request for '%s' with %zu bytes of arguments",
                 rpcName.c_str(), args.size());
    Logger::Trace("[RPCHandler::BuildRequest] exit — returning packet tag='RPC_CALL'");
    return pkt;
}

Packet RPCHandler::BuildResponse(const std::string& rpcName, const std::vector<uint8_t>& result) {
    Logger::Trace("[RPCHandler::BuildResponse] entry — rpcName='%s', result.size()=%zu bytes",
                  rpcName.c_str(), result.size());
    Packet pkt("RPC_RESPONSE");
    pkt.WriteString(rpcName);
    pkt.WriteUInt(static_cast<uint32_t>(result.size()));
    pkt.WriteBytes(result);
    Logger::Debug("[RPCHandler::BuildResponse] built RPC response packet: rpcName='%s', resultLen=%zu bytes, tag='RPC_RESPONSE'",
                  rpcName.c_str(), result.size());
    Logger::Info("[RPCHandler::BuildResponse] built RPC response for '%s' with %zu bytes of result data",
                 rpcName.c_str(), result.size());
    Logger::Trace("[RPCHandler::BuildResponse] exit — returning packet tag='RPC_RESPONSE'");
    return pkt;
}
