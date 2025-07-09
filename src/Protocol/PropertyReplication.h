// src/Protocol/PropertyReplication.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Network/Packet.h"

// Flags for which properties have changed
enum PropertyRepFlag : uint32_t {
    PR_NONE        = 0,
    PR_POSITION    = 1 << 0,
    PR_ORIENTATION = 1 << 1,
    PR_VELOCITY    = 1 << 2,
    PR_HEALTH      = 1 << 3,
    PR_STATE       = 1 << 4,
    PR_CUSTOM      = 1 << 5
};

struct PropertyState {
    uint32_t            objectId;
    uint32_t            flags;         // bitmask of PropertyRepFlag
    // Standard properties
    Vector3             position;
    Vector3             orientation;
    Vector3             velocity;
    int32_t             health;
    uint32_t            state;         // e.g., animation, alive/dead
    // Extension data
    std::vector<uint8_t> customData;
};

class PropertyReplication {
public:
    // Build a packet encoding all changed property states
    static Packet BuildPacket(const std::vector<PropertyState>& props);

    // Parse a received packet into a list of PropertyState
    static std::vector<PropertyState> ParsePacket(const Packet& pkt);
};