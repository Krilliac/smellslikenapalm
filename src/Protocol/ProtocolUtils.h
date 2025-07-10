#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "PacketTypes.h"    // RS2V PacketType enum

namespace ProtocolUtils {

    // Split a qualified tag "CATEGORY:SUBTYPE" into {category, subtype}
    std::pair<std::string,std::string> SplitTag(const std::string& tag);

    // Join category and subtype into a single tag
    std::string JoinTag(const std::string& category, const std::string& subtype);

    // Encode a vector of bytes as a hex string for logging/debug
    std::string ToHexString(const std::vector<uint8_t>& data);

    // Decode a hex string back into bytes; returns nullopt on invalid input
    std::optional<std::vector<uint8_t>> FromHexString(const std::string& hex);

    // Compute a simple XOR checksum over a payload
    uint8_t ComputeChecksum(const std::vector<uint8_t>& payload);

    // Verify checksum: last byte of payload is checksum; strips it if valid
    bool VerifyAndStripChecksum(std::vector<uint8_t>& payload);

    // Base64 encode/decode for embedding binary in text messages
    std::string Base64Encode(const std::vector<uint8_t>& data);
    std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64);

    // Map a string tag (Packet::GetTag()) to PacketType enum
    PacketType TagToType(const std::string& tag);

    // Convert a PacketType enum back to its tag string
    std::string TypeToTag(PacketType type);
}