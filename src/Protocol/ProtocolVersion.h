// src/Protocol/ProtocolVersion.h
#pragma once

#include <cstdint>
#include <string>

// Defines the network protocol version for RS2V server and clients.
// Increment this when any serialized packet formats, tags, or semantics change.
namespace ProtocolVersion {
    // Major version: incompatible changes (e.g., packet structure changes)
    constexpr uint16_t MAJOR = 1;
    // Minor version: backward-compatible additions (new optional fields, flags)
    constexpr uint16_t MINOR = 0;
    // Patch version: bug fixes without protocol additions
    constexpr uint16_t PATCH = 0;

    // Returns a combined 48-bit version: (major<<32)|(minor<<16)|patch
    inline uint64_t Combined() {
        return (uint64_t(MAJOR) << 32) | (uint64_t(MINOR) << 16) | uint64_t(PATCH);
    }

    // Returns "MAJOR.MINOR.PATCH" string
    inline std::string ToString() {
        return std::to_string(MAJOR) + "." +
               std::to_string(MINOR) + "." +
               std::to_string(PATCH);
    }
}