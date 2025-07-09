// src/Protocol/MessageEncoder.cpp
#include "Protocol/MessageEncoder.h"
#include "Network/Packet.h"

// Generic
Packet MessageEncoder::Encode(const std::string& tag, const std::vector<uint8_t>& payload) {
    return Packet(tag, payload);
}

// Chat: [clientId:uint32][text:string]
Packet MessageEncoder::EncodeChatMessage(uint32_t clientId, const std::string& text) {
    Packet pkt("CHAT_MESSAGE");
    pkt.WriteUInt(clientId);
    pkt.WriteString(text);
    return pkt;
}

// Movement: [clientId:uint32][x:float][y:float][z:float]
Packet MessageEncoder::EncodeMovement(uint32_t clientId, float x, float y, float z) {
    Packet pkt("MOVE");
    pkt.WriteUInt(clientId);
    pkt.WriteFloat(x);
    pkt.WriteFloat(y);
    pkt.WriteFloat(z);
    return pkt;
}

// Action: [clientId:uint32][action:string][argCount:uint32][args...]
Packet MessageEncoder::EncodeAction(uint32_t clientId,
                                    const std::string& action,
                                    const std::vector<std::string>& args)
{
    Packet pkt("PLAYER_ACTION");
    pkt.WriteUInt(clientId);
    pkt.WriteString(action);
    pkt.WriteUInt(static_cast<uint32_t>(args.size()));
    for (const auto& a : args) {
        pkt.WriteString(a);
    }
    return pkt;
}

// Actor replication: wrap pre-serialized payload
Packet MessageEncoder::EncodeActorReplication(const std::vector<uint8_t>& replicationPayload) {
    // reuse Packet constructor to attach payload
    return Packet("ACTOR_REPLICATION", replicationPayload);
}