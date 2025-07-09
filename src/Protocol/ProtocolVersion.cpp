// src/Protocol/ProtocolVersion.cpp
#include "Protocol/ProtocolVersion.h"
#include <sstream>

namespace ProtocolVersion {
    // Optionally implement runtime parsing or comparison helpers here

    bool IsCompatible(uint64_t remoteCombined) {
        // Compatible if major matches and remote minor ≥ local minor
        uint16_t remoteMajor = (remoteCombined >> 32) & 0xFFFF;
        uint16_t remoteMinor = (remoteCombined >> 16) & 0xFFFF;
        return (remoteMajor == MAJOR) && (remoteMinor >= MINOR);
    }

    std::string CombinedHex() {
        std::ostringstream oss;
        oss << std::hex << Combined();
        return oss.str();
    }
}