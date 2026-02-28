// src/Protocol/ActorReplication.cpp
#include "Protocol/ActorReplication.h"
#include "Utils/Logger.h"

// Helper to write ActorState to payload
static void WriteActorState(Packet& pkt, const ActorState& s) {
    Logger::Trace("[WriteActorState] entry — actorId=%u, stateFlags=0x%08X", s.actorId, s.stateFlags);
    pkt.WriteUInt(s.actorId);
    pkt.WriteUInt(s.stateFlags);

    if (s.stateFlags & ActorRepFlag::POSITION) {
        Logger::Debug("[WriteActorState] POSITION flag set — writing position (%.3f, %.3f, %.3f)",
                      s.position.x, s.position.y, s.position.z);
        pkt.WriteVector3(s.position);
    }
    if (s.stateFlags & ActorRepFlag::ORIENTATION) {
        Logger::Debug("[WriteActorState] ORIENTATION flag set — writing orientation (%.3f, %.3f, %.3f)",
                      s.orientation.x, s.orientation.y, s.orientation.z);
        pkt.WriteVector3(s.orientation);
    }
    if (s.stateFlags & ActorRepFlag::VELOCITY) {
        Logger::Debug("[WriteActorState] VELOCITY flag set — writing velocity (%.3f, %.3f, %.3f)",
                      s.velocity.x, s.velocity.y, s.velocity.z);
        pkt.WriteVector3(s.velocity);
    }
    if (s.stateFlags & ActorRepFlag::HEALTH) {
        Logger::Debug("[WriteActorState] HEALTH flag set — writing health=%d", s.health);
        pkt.WriteInt(s.health);
    }
    if (s.stateFlags & ActorRepFlag::STATE) {
        Logger::Debug("[WriteActorState] STATE flag set — writing additional stateFlags=0x%08X", s.stateFlags);
        pkt.WriteUInt(s.stateFlags); // additional bits if needed
    }
    if (s.stateFlags & ActorRepFlag::CUSTOM) {
        Logger::Debug("[WriteActorState] CUSTOM flag set — writing customData of %zu bytes",
                      s.customData.size());
        pkt.WriteUInt((uint32_t)s.customData.size());
        pkt.WriteBytes(s.customData);
    }
    Logger::Trace("[WriteActorState] exit — finished writing actorId=%u", s.actorId);
}

Packet ActorReplication::BuildReplicationPacket(const std::vector<ActorState>& states) {
    Logger::Trace("[ActorReplication::BuildReplicationPacket] entry — states.size()=%zu", states.size());
    Packet pkt("ACTOR_REPLICATION");
    pkt.WriteUInt((uint32_t)states.size());
    Logger::Debug("[ActorReplication::BuildReplicationPacket] wrote actor count=%u", (uint32_t)states.size());
    for (size_t i = 0; i < states.size(); ++i) {
        Logger::Trace("[ActorReplication::BuildReplicationPacket] serializing actor state %zu/%zu, actorId=%u",
                      i + 1, states.size(), states[i].actorId);
        WriteActorState(pkt, states[i]);
    }
    Logger::Info("[ActorReplication::BuildReplicationPacket] built replication packet with %zu actor states", states.size());
    Logger::Trace("[ActorReplication::BuildReplicationPacket] exit — returning packet tag='ACTOR_REPLICATION'");
    return pkt;
}

// Helper to read ActorState
static ActorState ReadActorState(Packet& pkt) {
    Logger::Trace("[ReadActorState] entry — reading next actor state from packet");
    ActorState s;
    s.actorId    = pkt.ReadUInt();
    s.stateFlags = pkt.ReadUInt();
    Logger::Debug("[ReadActorState] read actorId=%u, stateFlags=0x%08X", s.actorId, s.stateFlags);

    if (s.stateFlags & ActorRepFlag::POSITION) {
        s.position = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] POSITION flag set — read position (%.3f, %.3f, %.3f)",
                      s.position.x, s.position.y, s.position.z);
    }
    if (s.stateFlags & ActorRepFlag::ORIENTATION) {
        s.orientation = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] ORIENTATION flag set — read orientation (%.3f, %.3f, %.3f)",
                      s.orientation.x, s.orientation.y, s.orientation.z);
    }
    if (s.stateFlags & ActorRepFlag::VELOCITY) {
        s.velocity = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] VELOCITY flag set — read velocity (%.3f, %.3f, %.3f)",
                      s.velocity.x, s.velocity.y, s.velocity.z);
    }
    if (s.stateFlags & ActorRepFlag::HEALTH) {
        s.health = pkt.ReadInt();
        Logger::Debug("[ReadActorState] HEALTH flag set — read health=%d", s.health);
    }
    if (s.stateFlags & ActorRepFlag::STATE) {
        // already read stateFlags, skip or re-read if extended
        uint32_t extra = pkt.ReadUInt();
        Logger::Debug("[ReadActorState] STATE flag set — read extra state value=0x%08X (discarded)", extra);
        (void)extra;
    }
    if (s.stateFlags & ActorRepFlag::CUSTOM) {
        uint32_t len = pkt.ReadUInt();
        s.customData = pkt.ReadBytes(len);
        Logger::Debug("[ReadActorState] CUSTOM flag set — read %u bytes of custom data", len);
    }
    Logger::Trace("[ReadActorState] exit — returning actorId=%u with stateFlags=0x%08X", s.actorId, s.stateFlags);
    return s;
}

std::vector<ActorState> ActorReplication::ParseReplicationPacket(const Packet& pkt) {
    Logger::Trace("[ActorReplication::ParseReplicationPacket] entry — packet tag='%s'", pkt.GetTag().c_str());
    Packet copy = pkt; // make mutable copy for reading
    uint32_t count = copy.ReadUInt();
    Logger::Debug("[ActorReplication::ParseReplicationPacket] actor count=%u", count);
    std::vector<ActorState> list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Logger::Trace("[ActorReplication::ParseReplicationPacket] parsing actor state %u/%u", i + 1, count);
        list.push_back(ReadActorState(copy));
    }
    Logger::Info("[ActorReplication::ParseReplicationPacket] parsed %u actor states from replication packet", count);
    Logger::Trace("[ActorReplication::ParseReplicationPacket] exit — returning %zu actor states", list.size());
    return list;
}
