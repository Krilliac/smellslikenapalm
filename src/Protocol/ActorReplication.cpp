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

// Sizes of the fixed-width fields read from the wire (the underlying legacy
// Packet::ReadUInt/ReadInt/ReadFloat/ReadVector3/ReadBytes accessors are NOT
// bounds-checked and would memcpy past the end of the payload on a short or
// malformed/attacker-controlled datagram). We validate BytesRemaining() at
// each trust boundary before invoking them.
static const size_t kU32Size    = 4;             // uint32 / int32
static const size_t kVec3Size   = 4 * 3;         // three floats
static const size_t kMinStateHdr = kU32Size * 2; // actorId + stateFlags

// Helper to read ActorState. On a truncated/malformed packet `ok` is set false
// and the partially-populated state is returned so the caller can stop without
// triggering an out-of-bounds read. Valid input is unaffected.
static ActorState ReadActorState(Packet& pkt, bool& ok) {
    Logger::Trace("[ReadActorState] entry — reading next actor state from packet");
    ok = true;
    ActorState s{}; // zero-init: fields whose flag is unset stay defined, never garbage

    // actorId + stateFlags header.
    if (pkt.BytesRemaining() < kMinStateHdr) {
        Logger::Warn("[ReadActorState] truncated packet: need %zu bytes for actor header, have %zu — rejecting",
                     kMinStateHdr, pkt.BytesRemaining());
        ok = false;
        return s;
    }
    s.actorId    = pkt.ReadUInt();
    s.stateFlags = pkt.ReadUInt();
    Logger::Debug("[ReadActorState] read actorId=%u, stateFlags=0x%08X", s.actorId, s.stateFlags);

    if (s.stateFlags & ActorRepFlag::POSITION) {
        if (pkt.BytesRemaining() < kVec3Size) {
            Logger::Warn("[ReadActorState] truncated packet: POSITION needs %zu bytes, have %zu — rejecting",
                         kVec3Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        s.position = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] POSITION flag set — read position (%.3f, %.3f, %.3f)",
                      s.position.x, s.position.y, s.position.z);
    }
    if (s.stateFlags & ActorRepFlag::ORIENTATION) {
        if (pkt.BytesRemaining() < kVec3Size) {
            Logger::Warn("[ReadActorState] truncated packet: ORIENTATION needs %zu bytes, have %zu — rejecting",
                         kVec3Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        s.orientation = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] ORIENTATION flag set — read orientation (%.3f, %.3f, %.3f)",
                      s.orientation.x, s.orientation.y, s.orientation.z);
    }
    if (s.stateFlags & ActorRepFlag::VELOCITY) {
        if (pkt.BytesRemaining() < kVec3Size) {
            Logger::Warn("[ReadActorState] truncated packet: VELOCITY needs %zu bytes, have %zu — rejecting",
                         kVec3Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        s.velocity = pkt.ReadVector3();
        Logger::Debug("[ReadActorState] VELOCITY flag set — read velocity (%.3f, %.3f, %.3f)",
                      s.velocity.x, s.velocity.y, s.velocity.z);
    }
    if (s.stateFlags & ActorRepFlag::HEALTH) {
        if (pkt.BytesRemaining() < kU32Size) {
            Logger::Warn("[ReadActorState] truncated packet: HEALTH needs %zu bytes, have %zu — rejecting",
                         kU32Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        s.health = pkt.ReadInt();
        Logger::Debug("[ReadActorState] HEALTH flag set — read health=%d", s.health);
    }
    if (s.stateFlags & ActorRepFlag::STATE) {
        // already read stateFlags, skip or re-read if extended
        if (pkt.BytesRemaining() < kU32Size) {
            Logger::Warn("[ReadActorState] truncated packet: STATE needs %zu bytes, have %zu — rejecting",
                         kU32Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        uint32_t extra = pkt.ReadUInt();
        Logger::Debug("[ReadActorState] STATE flag set — read extra state value=0x%08X (discarded)", extra);
        (void)extra;
    }
    if (s.stateFlags & ActorRepFlag::CUSTOM) {
        if (pkt.BytesRemaining() < kU32Size) {
            Logger::Warn("[ReadActorState] truncated packet: CUSTOM length prefix needs %zu bytes, have %zu — rejecting",
                         kU32Size, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        uint32_t len = pkt.ReadUInt();
        // len is attacker-controlled; reject if it claims more than the packet holds
        // (the unchecked ReadBytes would otherwise read/allocate out of bounds).
        if (len > pkt.BytesRemaining()) {
            Logger::Warn("[ReadActorState] CUSTOM length %u exceeds %zu remaining bytes — rejecting",
                         len, pkt.BytesRemaining());
            ok = false;
            return s;
        }
        s.customData = pkt.ReadBytes(len);
        Logger::Debug("[ReadActorState] CUSTOM flag set — read %u bytes of custom data", len);
    }
    Logger::Trace("[ReadActorState] exit — returning actorId=%u with stateFlags=0x%08X", s.actorId, s.stateFlags);
    return s;
}

std::vector<ActorState> ActorReplication::ParseReplicationPacket(const Packet& pkt) {
    Logger::Trace("[ActorReplication::ParseReplicationPacket] entry — packet tag='%s'", pkt.GetTag().c_str());
    Packet copy = pkt; // make mutable copy for reading
    std::vector<ActorState> list;

    // The 4-byte actor-count prefix is attacker-controlled. ReadUInt is not
    // bounds-checked, so verify the payload actually has those 4 bytes first.
    if (copy.BytesRemaining() < kU32Size) {
        Logger::Warn("[ActorReplication::ParseReplicationPacket] payload too small for actor-count prefix (have %zu bytes) — returning empty list",
                     copy.BytesRemaining());
        return list;
    }
    uint32_t count = copy.ReadUInt();
    Logger::Debug("[ActorReplication::ParseReplicationPacket] actor count=%u", count);

    // Clamp the reservation: each actor state needs at least kMinStateHdr bytes,
    // so a payload can never hold more than (remaining / kMinStateHdr) actors.
    // This prevents an attacker-supplied count (up to ~4 billion) from driving a
    // huge speculative allocation. The loop still honours `count` for valid
    // input (where count <= maxPossible), so decode output is unchanged.
    const size_t maxPossible = copy.BytesRemaining() / kMinStateHdr;
    const size_t toReserve = ((size_t)count < maxPossible) ? (size_t)count : maxPossible;
    if ((size_t)count > maxPossible) {
        Logger::Warn("[ActorReplication::ParseReplicationPacket] declared actor count=%u exceeds max possible=%zu for %zu remaining bytes — will stop early on exhaustion",
                     count, maxPossible, copy.BytesRemaining());
    }
    list.reserve(toReserve);

    for (uint32_t i = 0; i < count; ++i) {
        Logger::Trace("[ActorReplication::ParseReplicationPacket] parsing actor state %u/%u", i + 1, count);
        bool ok = false;
        ActorState st = ReadActorState(copy, ok);
        if (!ok) {
            Logger::Warn("[ActorReplication::ParseReplicationPacket] stopping after %zu of %u declared actor states (packet exhausted/malformed)",
                         list.size(), count);
            break;
        }
        list.push_back(std::move(st));
    }
    Logger::Info("[ActorReplication::ParseReplicationPacket] parsed %zu actor states from replication packet", list.size());
    Logger::Trace("[ActorReplication::ParseReplicationPacket] exit — returning %zu actor states", list.size());
    return list;
}
