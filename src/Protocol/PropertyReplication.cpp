// src/Protocol/PropertyReplication.cpp
#include "Protocol/PropertyReplication.h"
#include "Utils/Logger.h"

// Helper to write one PropertyState into packet
static void WritePropertyState(Packet& pkt, const PropertyState& ps) {
    Logger::Trace("[WritePropertyState] entry — objectId=%u, flags=0x%08X", ps.objectId, ps.flags);
    pkt.WriteUInt(ps.objectId);
    pkt.WriteUInt(ps.flags);

    if (ps.flags & PR_POSITION) {
        Logger::Debug("[WritePropertyState] PR_POSITION flag set — writing position (%.3f, %.3f, %.3f)",
                      ps.position.x, ps.position.y, ps.position.z);
        pkt.WriteVector3(ps.position);
    }
    if (ps.flags & PR_ORIENTATION) {
        Logger::Debug("[WritePropertyState] PR_ORIENTATION flag set — writing orientation (%.3f, %.3f, %.3f)",
                      ps.orientation.x, ps.orientation.y, ps.orientation.z);
        pkt.WriteVector3(ps.orientation);
    }
    if (ps.flags & PR_VELOCITY) {
        Logger::Debug("[WritePropertyState] PR_VELOCITY flag set — writing velocity (%.3f, %.3f, %.3f)",
                      ps.velocity.x, ps.velocity.y, ps.velocity.z);
        pkt.WriteVector3(ps.velocity);
    }
    if (ps.flags & PR_HEALTH) {
        Logger::Debug("[WritePropertyState] PR_HEALTH flag set — writing health=%d", ps.health);
        pkt.WriteInt(ps.health);
    }
    if (ps.flags & PR_STATE) {
        Logger::Debug("[WritePropertyState] PR_STATE flag set — writing state=0x%08X", ps.state);
        pkt.WriteUInt(ps.state);
    }
    if (ps.flags & PR_CUSTOM) {
        Logger::Debug("[WritePropertyState] PR_CUSTOM flag set — writing %zu bytes of custom data",
                      ps.customData.size());
        pkt.WriteUInt((uint32_t)ps.customData.size());
        pkt.WriteBytes(ps.customData);
    }
    Logger::Trace("[WritePropertyState] exit — finished writing objectId=%u", ps.objectId);
}

Packet PropertyReplication::BuildPacket(const std::vector<PropertyState>& props) {
    Logger::Trace("[PropertyReplication::BuildPacket] entry — props.size()=%zu", props.size());
    Packet pkt("PROPERTY_REPLICATION");
    pkt.WriteUInt((uint32_t)props.size());
    Logger::Debug("[PropertyReplication::BuildPacket] wrote property count=%u", (uint32_t)props.size());
    for (size_t i = 0; i < props.size(); ++i) {
        Logger::Trace("[PropertyReplication::BuildPacket] serializing property state %zu/%zu, objectId=%u",
                      i + 1, props.size(), props[i].objectId);
        WritePropertyState(pkt, props[i]);
    }
    Logger::Info("[PropertyReplication::BuildPacket] built property replication packet with %zu property states",
                 props.size());
    Logger::Trace("[PropertyReplication::BuildPacket] exit — returning packet tag='PROPERTY_REPLICATION'");
    return pkt;
}

// Wire-size constants for trust-boundary bounds checks. The legacy Packet
// Read* helpers (ReadUInt/ReadInt/ReadFloat/ReadVector3/ReadBytes) do NOT
// bounds-check and index m_payload directly, so a malformed/attacker-supplied
// PROPERTY_REPLICATION payload could otherwise drive an out-of-bounds read,
// an unbounded allocation, or an unbounded parse loop. These guards validate
// every field against BytesRemaining() BEFORE reading. They never change the
// decode result for a valid payload (a valid payload always has the bytes the
// guard requires) and never throw — on short/oversized input they LOG + reject.
static constexpr size_t kSizeU32     = 4;  // one uint32_t / int32_t field
static constexpr size_t kSizeVector3 = 12; // three float components
static constexpr size_t kStateMinBytes = kSizeU32 + kSizeU32; // objectId + flags

// Helper to read one PropertyState from packet.
// Returns false (and leaves *ps partially populated) if the packet runs out of
// bytes for a field that the flags say should be present; the caller stops.
static bool ReadPropertyState(Packet& pkt, PropertyState& ps) {
    Logger::Trace("[ReadPropertyState] entry — reading next property state from packet");

    if (pkt.BytesRemaining() < kStateMinBytes) {
        Logger::Warn("[ReadPropertyState] truncated header: need %zu bytes for objectId+flags, have %zu — rejecting",
                     kStateMinBytes, pkt.BytesRemaining());
        return false;
    }
    ps.objectId = pkt.ReadUInt();
    ps.flags    = pkt.ReadUInt();
    Logger::Debug("[ReadPropertyState] read objectId=%u, flags=0x%08X", ps.objectId, ps.flags);

    if (ps.flags & PR_POSITION) {
        if (pkt.BytesRemaining() < kSizeVector3) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_POSITION (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeVector3, pkt.BytesRemaining());
            return false;
        }
        ps.position = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_POSITION flag set — read position (%.3f, %.3f, %.3f)",
                      ps.position.x, ps.position.y, ps.position.z);
    }
    if (ps.flags & PR_ORIENTATION) {
        if (pkt.BytesRemaining() < kSizeVector3) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_ORIENTATION (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeVector3, pkt.BytesRemaining());
            return false;
        }
        ps.orientation = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_ORIENTATION flag set — read orientation (%.3f, %.3f, %.3f)",
                      ps.orientation.x, ps.orientation.y, ps.orientation.z);
    }
    if (ps.flags & PR_VELOCITY) {
        if (pkt.BytesRemaining() < kSizeVector3) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_VELOCITY (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeVector3, pkt.BytesRemaining());
            return false;
        }
        ps.velocity = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_VELOCITY flag set — read velocity (%.3f, %.3f, %.3f)",
                      ps.velocity.x, ps.velocity.y, ps.velocity.z);
    }
    if (ps.flags & PR_HEALTH) {
        if (pkt.BytesRemaining() < kSizeU32) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_HEALTH (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeU32, pkt.BytesRemaining());
            return false;
        }
        ps.health = pkt.ReadInt();
        Logger::Debug("[ReadPropertyState] PR_HEALTH flag set — read health=%d", ps.health);
    }
    if (ps.flags & PR_STATE) {
        if (pkt.BytesRemaining() < kSizeU32) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_STATE (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeU32, pkt.BytesRemaining());
            return false;
        }
        ps.state = pkt.ReadUInt();
        Logger::Debug("[ReadPropertyState] PR_STATE flag set — read state=0x%08X", ps.state);
    }
    if (ps.flags & PR_CUSTOM) {
        if (pkt.BytesRemaining() < kSizeU32) {
            Logger::Warn("[ReadPropertyState] objectId=%u: truncated PR_CUSTOM length prefix (need %zu, have %zu) — rejecting",
                         ps.objectId, kSizeU32, pkt.BytesRemaining());
            return false;
        }
        uint32_t len = pkt.ReadUInt();
        // The length prefix is attacker-controlled; ReadBytes() does not range
        // check, so verify the bytes actually exist before reading/allocating.
        if (len > pkt.BytesRemaining()) {
            Logger::Warn("[ReadPropertyState] objectId=%u: PR_CUSTOM length %u exceeds %zu remaining bytes — rejecting",
                         ps.objectId, len, pkt.BytesRemaining());
            return false;
        }
        ps.customData = pkt.ReadBytes(len);
        Logger::Debug("[ReadPropertyState] PR_CUSTOM flag set — read %u bytes of custom data", len);
    }
    Logger::Trace("[ReadPropertyState] exit — returning objectId=%u with flags=0x%08X", ps.objectId, ps.flags);
    return true;
}

std::vector<PropertyState> PropertyReplication::ParsePacket(const Packet& pkt) {
    Logger::Trace("[PropertyReplication::ParsePacket] entry — packet tag='%s'", pkt.GetTag().c_str());
    // Make mutable copy for reading
    Packet reader = pkt;

    std::vector<PropertyState> list;

    // Guard the count prefix itself: ReadUInt() does not bounds-check.
    if (reader.BytesRemaining() < kSizeU32) {
        Logger::Warn("[PropertyReplication::ParsePacket] payload too small for count prefix (have %zu) — returning empty",
                     reader.BytesRemaining());
        return list;
    }
    uint32_t count = reader.ReadUInt();
    Logger::Debug("[PropertyReplication::ParsePacket] property count=%u", count);

    // Each PropertyState occupies at least kStateMinBytes (objectId + flags), so
    // a count larger than (remaining / kStateMinBytes) is impossible for a
    // well-formed payload. Clamp the reserve hint to that upper bound to avoid an
    // attacker driving a multi-gigabyte allocation via an inflated count. The
    // loop below still stops at the first field that runs out of bytes, so valid
    // payloads are parsed identically.
    const size_t maxPossible = reader.BytesRemaining() / kStateMinBytes;
    if (count > maxPossible) {
        Logger::Warn("[PropertyReplication::ParsePacket] count=%u exceeds max possible %zu for %zu remaining bytes — clamping parse",
                     count, maxPossible, reader.BytesRemaining());
    }
    list.reserve(static_cast<size_t>(count) < maxPossible ? static_cast<size_t>(count) : maxPossible);

    for (uint32_t i = 0; i < count; ++i) {
        Logger::Trace("[PropertyReplication::ParsePacket] parsing property state %u/%u", i + 1, count);
        PropertyState ps;
        if (!ReadPropertyState(reader, ps)) {
            Logger::Warn("[PropertyReplication::ParsePacket] stopping after %zu/%u states — packet exhausted or malformed",
                         list.size(), count);
            break;
        }
        list.push_back(std::move(ps));
    }
    Logger::Info("[PropertyReplication::ParsePacket] parsed %zu property states from packet (declared count=%u)",
                 list.size(), count);
    Logger::Trace("[PropertyReplication::ParsePacket] exit — returning %zu property states", list.size());
    return list;
}
