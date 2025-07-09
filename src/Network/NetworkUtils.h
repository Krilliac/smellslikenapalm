// src/Network/NetworkUtils.h

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace NetworkUtils {

    // Normalize an IPv4 address string (e.g., "127.000.000.001" → "127.0.0.1")
    std::optional<std::string> NormalizeIPv4(const std::string& ip);

    // Validate that a string is a valid IPv4 address
    bool IsValidIPv4(const std::string& ip);

    // Split a string by delimiter into vector of tokens
    std::vector<std::string> SplitString(const std::string& s, char delimiter);

    // Join tokens into a single string with delimiter
    std::string JoinString(const std::vector<std::string>& tokens, char delimiter);

    // Convert host-byte-order 16-bit port to network-byte-order and vice versa
    uint16_t HostToNetworkPort(uint16_t port);
    uint16_t NetworkToHostPort(uint16_t port);

    // Compute simple checksum (XOR of all bytes) for a data buffer
    uint8_t ComputeChecksum(const uint8_t* data, size_t length);

}