// src/Protocol/ActorReplication.h
#pragma once

#include <cstdint>
#include <vector>
#include "Math/Vector3.h"
#include "Network/Packet.h"

// Flags for replication fields
enum ActorRepFlag : uint32_t {
    POSITION    = 1 << 0,
    ORIENTATION = 1 << 1,
    VELOCITY    = 1 << 2,
    HEALTH      = 1 << 3,
    STATE       = 1 << 4,
    CUSTOM      = 1 << 5
};

struct ActorState {
    uint32_t    actorId;
    Vector3     position;
    Vector3     orientation;    // pitch,yaw,roll
    Vector3     velocity;
    int32_t     health;
    uint32_t    stateFlags;     // e.g., alive/dead, animation state
    std::vector<uint8_t> customData; // extension payload
};

class ActorReplication {
public:
    // Build a replication packet containing all changed actor states
    static Packet BuildReplicationPacket(const std::vector<ActorState>& states);

    // Parse incoming replication packet into ActorState list
    static std::vector<ActorState> ParseReplicationPacket(const Packet& pkt);
};