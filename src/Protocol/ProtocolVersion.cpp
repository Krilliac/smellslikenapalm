// src/Protocol/ProtocolVersion.cpp
#include "Protocol/ProtocolVersion.h"
#include "Utils/Logger.h"
#include <sstream>

namespace ProtocolVersion {
    // Optionally implement runtime parsing or comparison helpers here

    bool IsCompatible(uint64_t remoteCombined) {
        Logger::Trace("[ProtocolVersion::IsCompatible] entry — remoteCombined=0x%016llX",
                      (unsigned long long)remoteCombined);
        // Compatible if major matches and remote minor >= local minor
        uint16_t remoteMajor = (remoteCombined >> 32) & 0xFFFF;
        uint16_t remoteMinor = (remoteCombined >> 16) & 0xFFFF;
        Logger::Debug("[ProtocolVersion::IsCompatible] parsed remoteMajor=%u, remoteMinor=%u — local MAJOR=%u, MINOR=%u",
                      remoteMajor, remoteMinor, MAJOR, MINOR);
        bool compatible = (remoteMajor == MAJOR) && (remoteMinor >= MINOR);
        if (compatible) {
            Logger::Info("[ProtocolVersion::IsCompatible] remote protocol version %u.%u is compatible with local %u.%u",
                         remoteMajor, remoteMinor, MAJOR, MINOR);
        } else {
            Logger::Warn("[ProtocolVersion::IsCompatible] remote protocol version %u.%u is NOT compatible with local %u.%u",
                         remoteMajor, remoteMinor, MAJOR, MINOR);
            if (remoteMajor != MAJOR) {
                Logger::Debug("[ProtocolVersion::IsCompatible] major version mismatch: remote=%u, local=%u",
                              remoteMajor, MAJOR);
            }
            if (remoteMinor < MINOR) {
                Logger::Debug("[ProtocolVersion::IsCompatible] remote minor version %u is less than required local minor %u",
                              remoteMinor, MINOR);
            }
        }
        Logger::Trace("[ProtocolVersion::IsCompatible] exit — returning %s", compatible ? "true" : "false");
        return compatible;
    }

    std::string CombinedHex() {
        Logger::Trace("[ProtocolVersion::CombinedHex] entry");
        std::ostringstream oss;
        oss << std::hex << Combined();
        std::string result = oss.str();
        Logger::Debug("[ProtocolVersion::CombinedHex] combined version value=0x%s (raw=%llu)",
                      result.c_str(), (unsigned long long)Combined());
        Logger::Trace("[ProtocolVersion::CombinedHex] exit — returning '%s'", result.c_str());
        return result;
    }
}
