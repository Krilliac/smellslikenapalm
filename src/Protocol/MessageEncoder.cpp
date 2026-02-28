// src/Protocol/MessageEncoder.cpp
#include "Protocol/MessageEncoder.h"
#include "Network/Packet.h"
#include "Utils/Logger.h"

// Generic
Packet MessageEncoder::Encode(const std::string& tag, const std::vector<uint8_t>& payload) {
    Logger::Trace("[MessageEncoder::Encode] entry — tag='%s', payload size=%zu bytes", tag.c_str(), payload.size());
    Packet pkt(tag, payload);
    Logger::Info("[MessageEncoder::Encode] encoded generic packet with tag='%s', %zu bytes payload",
                 tag.c_str(), payload.size());
    Logger::Trace("[MessageEncoder::Encode] exit — returning packet");
    return pkt;
}

// Chat: [clientId:uint32][text:string]
Packet MessageEncoder::EncodeChatMessage(uint32_t clientId, const std::string& text) {
    Logger::Trace("[MessageEncoder::EncodeChatMessage] entry — clientId=%u, text='%s' (length=%zu)",
                  clientId, text.c_str(), text.size());
    Packet pkt("CHAT_MESSAGE");
    pkt.WriteUInt(clientId);
    pkt.WriteString(text);
    Logger::Debug("[MessageEncoder::EncodeChatMessage] wrote clientId=%u and text of %zu chars", clientId, text.size());
    Logger::Info("[MessageEncoder::EncodeChatMessage] encoded chat message from client %u", clientId);
    Logger::Trace("[MessageEncoder::EncodeChatMessage] exit — returning packet tag='CHAT_MESSAGE'");
    return pkt;
}

// Movement: [clientId:uint32][x:float][y:float][z:float]
Packet MessageEncoder::EncodeMovement(uint32_t clientId, float x, float y, float z) {
    Logger::Trace("[MessageEncoder::EncodeMovement] entry — clientId=%u, x=%.3f, y=%.3f, z=%.3f",
                  clientId, x, y, z);
    Packet pkt("MOVE");
    pkt.WriteUInt(clientId);
    pkt.WriteFloat(x);
    pkt.WriteFloat(y);
    pkt.WriteFloat(z);
    Logger::Debug("[MessageEncoder::EncodeMovement] wrote clientId=%u and position (%.3f, %.3f, %.3f)",
                  clientId, x, y, z);
    Logger::Info("[MessageEncoder::EncodeMovement] encoded movement for client %u to (%.3f, %.3f, %.3f)",
                 clientId, x, y, z);
    Logger::Trace("[MessageEncoder::EncodeMovement] exit — returning packet tag='MOVE'");
    return pkt;
}

// Action: [clientId:uint32][action:string][argCount:uint32][args...]
Packet MessageEncoder::EncodeAction(uint32_t clientId,
                                    const std::string& action,
                                    const std::vector<std::string>& args)
{
    Logger::Trace("[MessageEncoder::EncodeAction] entry — clientId=%u, action='%s', args.size()=%zu",
                  clientId, action.c_str(), args.size());
    Packet pkt("PLAYER_ACTION");
    pkt.WriteUInt(clientId);
    pkt.WriteString(action);
    pkt.WriteUInt(static_cast<uint32_t>(args.size()));
    for (size_t i = 0; i < args.size(); ++i) {
        Logger::Trace("[MessageEncoder::EncodeAction] writing arg[%zu]='%s'", i, args[i].c_str());
        pkt.WriteString(args[i]);
    }
    Logger::Debug("[MessageEncoder::EncodeAction] wrote clientId=%u, action='%s', %zu args",
                  clientId, action.c_str(), args.size());
    Logger::Info("[MessageEncoder::EncodeAction] encoded action '%s' for client %u with %zu args",
                 action.c_str(), clientId, args.size());
    Logger::Trace("[MessageEncoder::EncodeAction] exit — returning packet tag='PLAYER_ACTION'");
    return pkt;
}

// Actor replication: wrap pre-serialized payload
Packet MessageEncoder::EncodeActorReplication(const std::vector<uint8_t>& replicationPayload) {
    Logger::Trace("[MessageEncoder::EncodeActorReplication] entry — replicationPayload size=%zu bytes",
                  replicationPayload.size());
    // reuse Packet constructor to attach payload
    Packet pkt("ACTOR_REPLICATION", replicationPayload);
    Logger::Info("[MessageEncoder::EncodeActorReplication] encoded actor replication packet with %zu bytes payload",
                 replicationPayload.size());
    Logger::Trace("[MessageEncoder::EncodeActorReplication] exit — returning packet tag='ACTOR_REPLICATION'");
    return pkt;
}
