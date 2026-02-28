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

// Helper to read one PropertyState from packet
static PropertyState ReadPropertyState(Packet& pkt) {
    Logger::Trace("[ReadPropertyState] entry — reading next property state from packet");
    PropertyState ps;
    ps.objectId = pkt.ReadUInt();
    ps.flags    = pkt.ReadUInt();
    Logger::Debug("[ReadPropertyState] read objectId=%u, flags=0x%08X", ps.objectId, ps.flags);

    if (ps.flags & PR_POSITION) {
        ps.position = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_POSITION flag set — read position (%.3f, %.3f, %.3f)",
                      ps.position.x, ps.position.y, ps.position.z);
    }
    if (ps.flags & PR_ORIENTATION) {
        ps.orientation = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_ORIENTATION flag set — read orientation (%.3f, %.3f, %.3f)",
                      ps.orientation.x, ps.orientation.y, ps.orientation.z);
    }
    if (ps.flags & PR_VELOCITY) {
        ps.velocity = pkt.ReadVector3();
        Logger::Debug("[ReadPropertyState] PR_VELOCITY flag set — read velocity (%.3f, %.3f, %.3f)",
                      ps.velocity.x, ps.velocity.y, ps.velocity.z);
    }
    if (ps.flags & PR_HEALTH) {
        ps.health = pkt.ReadInt();
        Logger::Debug("[ReadPropertyState] PR_HEALTH flag set — read health=%d", ps.health);
    }
    if (ps.flags & PR_STATE) {
        ps.state = pkt.ReadUInt();
        Logger::Debug("[ReadPropertyState] PR_STATE flag set — read state=0x%08X", ps.state);
    }
    if (ps.flags & PR_CUSTOM) {
        uint32_t len = pkt.ReadUInt();
        ps.customData = pkt.ReadBytes(len);
        Logger::Debug("[ReadPropertyState] PR_CUSTOM flag set — read %u bytes of custom data", len);
    }
    Logger::Trace("[ReadPropertyState] exit — returning objectId=%u with flags=0x%08X", ps.objectId, ps.flags);
    return ps;
}

std::vector<PropertyState> PropertyReplication::ParsePacket(const Packet& pkt) {
    Logger::Trace("[PropertyReplication::ParsePacket] entry — packet tag='%s'", pkt.GetTag().c_str());
    // Make mutable copy for reading
    Packet reader = pkt;
    uint32_t count = reader.ReadUInt();
    Logger::Debug("[PropertyReplication::ParsePacket] property count=%u", count);
    std::vector<PropertyState> list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Logger::Trace("[PropertyReplication::ParsePacket] parsing property state %u/%u", i + 1, count);
        list.push_back(ReadPropertyState(reader));
    }
    Logger::Info("[PropertyReplication::ParsePacket] parsed %u property states from packet", count);
    Logger::Trace("[PropertyReplication::ParsePacket] exit — returning %zu property states", list.size());
    return list;
}
