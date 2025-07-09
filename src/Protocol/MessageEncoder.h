// src/Protocol/MessageEncoder.h
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "Network/Packet.h"

class MessageEncoder {
public:
    // Build a generic packet with tag and raw payload
    static Packet Encode(const std::string& tag, const std::vector<uint8_t>& payload);

    // Specialized encoders for common message types
    static Packet EncodeChatMessage(uint32_t clientId, const std::string& text);
    static Packet EncodeMovement(uint32_t clientId, float x, float y, float z);
    static Packet EncodeAction(uint32_t clientId, const std::string& action, const std::vector<std::string>& args);
    
    // Add extension: encode replication, state updates, etc.
    static Packet EncodeActorReplication(const std::vector<uint8_t>& replicationPayload);
};