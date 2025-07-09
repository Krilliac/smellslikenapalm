// src/Protocol/MessageDecoder.h
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "Network/Packet.h"

class MessageDecoder {
public:
    // Decode raw packet payload into high-level message structure
    // Returns true on success, false on malformed data
    static bool Decode(const Packet& pkt, std::string& outTag, std::vector<uint8_t>& outPayload);

    // Specialized decoders for common message types
    static bool DecodeChatMessage(const Packet& pkt, uint32_t& outClientId, std::string& outText);
    static bool DecodeMovement(const Packet& pkt, uint32_t& outClientId, float& outX, float& outY, float& outZ);
    static bool DecodeAction(const Packet& pkt, uint32_t& outClientId, std::string& outAction, std::vector<std::string>& outArgs);
};