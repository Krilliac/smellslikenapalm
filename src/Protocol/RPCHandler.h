// src/Protocol/RPCHandler.h
#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/MessageDecoder.h"
#include "Protocol/MessageEncoder.h"

struct PacketMetadata;

class RPCHandler {
public:
    // RPC function signature: clientId, args payload, metadata → optional return Packet
    using RPCFunc = std::function<std::optional<Packet>(uint32_t, const Packet&, const PacketMetadata&)>;

    RPCHandler();
    ~RPCHandler();

    // Register a server‐side RPC by name
    void RegisterRPC(const std::string& name, RPCFunc func);

    // Call this on incoming packets; dispatches to correct RPC and sends response if any
    void Handle(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta,
                std::function<void(const Packet&)> sendFunc);

    // Build an RPC request packet to send to client
    static Packet BuildRequest(const std::string& rpcName, const std::vector<uint8_t>& args);

    // Build an RPC response packet to send to server
    static Packet BuildResponse(const std::string& rpcName, const std::vector<uint8_t>& result);

private:
    std::unordered_map<std::string, RPCFunc> m_rpcMap;
    std::string m_requestTag   = "RPC_CALL";
    std::string m_responseTag  = "RPC_RESPONSE";
};