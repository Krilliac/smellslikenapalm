// src/Protocol/ActorReplication.cpp
#include "Protocol/ActorReplication.h"
#include "Utils/Logger.h"

// Helper to write ActorState to payload
static void WriteActorState(Packet& pkt, const ActorState& s) {
    pkt.WriteUInt(s.actorId);
    pkt.WriteUInt(s.stateFlags);

    if (s.stateFlags & ActorRepFlag::POSITION) {
        pkt.WriteVector3(s.position);
    }
    if (s.stateFlags & ActorRepFlag::ORIENTATION) {
        pkt.WriteVector3(s.orientation);
    }
    if (s.stateFlags & ActorRepFlag::VELOCITY) {
        pkt.WriteVector3(s.velocity);
    }
    if (s.stateFlags & ActorRepFlag::HEALTH) {
        pkt.WriteInt(s.health);
    }
    if (s.stateFlags & ActorRepFlag::STATE) {
        pkt.WriteUInt(s.stateFlags); // additional bits if needed
    }
    if (s.stateFlags & ActorRepFlag::CUSTOM) {
        pkt.WriteUInt((uint32_t)s.customData.size());
        pkt.WriteBytes(s.customData);
    }
}

Packet ActorReplication::BuildReplicationPacket(const std::vector<ActorState>& states) {
    Packet pkt("ACTOR_REPLICATION");
    pkt.WriteUInt((uint32_t)states.size());
    for (auto& s : states) {
        WriteActorState(pkt, s);
    }
    return pkt;
}

// Helper to read ActorState
static ActorState ReadActorState(Packet& pkt) {
    ActorState s;
    s.actorId    = pkt.ReadUInt();
    s.stateFlags = pkt.ReadUInt();

    if (s.stateFlags & ActorRepFlag::POSITION) {
        s.position = pkt.ReadVector3();
    }
    if (s.stateFlags & ActorRepFlag::ORIENTATION) {
        s.orientation = pkt.ReadVector3();
    }
    if (s.stateFlags & ActorRepFlag::VELOCITY) {
        s.velocity = pkt.ReadVector3();
    }
    if (s.stateFlags & ActorRepFlag::HEALTH) {
        s.health = pkt.ReadInt();
    }
    if (s.stateFlags & ActorRepFlag::STATE) {
        // already read stateFlags, skip or re-read if extended
        uint32_t extra = pkt.ReadUInt();
        (void)extra;
    }
    if (s.stateFlags & ActorRepFlag::CUSTOM) {
        uint32_t len = pkt.ReadUInt();
        s.customData = pkt.ReadBytes(len);
    }
    return s;
}

std::vector<ActorState> ActorReplication::ParseReplicationPacket(const Packet& pkt) {
    Packet copy = pkt; // make mutable copy for reading
    uint32_t count = copy.ReadUInt();
    std::vector<ActorState> list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        list.push_back(ReadActorState(copy));
    }
    return list;
}