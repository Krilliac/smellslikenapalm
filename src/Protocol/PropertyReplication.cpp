// src/Protocol/PropertyReplication.cpp
#include "Protocol/PropertyReplication.h"

// Helper to write one PropertyState into packet
static void WritePropertyState(Packet& pkt, const PropertyState& ps) {
    pkt.WriteUInt(ps.objectId);
    pkt.WriteUInt(ps.flags);

    if (ps.flags & PR_POSITION) {
        pkt.WriteVector3(ps.position);
    }
    if (ps.flags & PR_ORIENTATION) {
        pkt.WriteVector3(ps.orientation);
    }
    if (ps.flags & PR_VELOCITY) {
        pkt.WriteVector3(ps.velocity);
    }
    if (ps.flags & PR_HEALTH) {
        pkt.WriteInt(ps.health);
    }
    if (ps.flags & PR_STATE) {
        pkt.WriteUInt(ps.state);
    }
    if (ps.flags & PR_CUSTOM) {
        pkt.WriteUInt((uint32_t)ps.customData.size());
        pkt.WriteBytes(ps.customData);
    }
}

Packet PropertyReplication::BuildPacket(const std::vector<PropertyState>& props) {
    Packet pkt("PROPERTY_REPLICATION");
    pkt.WriteUInt((uint32_t)props.size());
    for (const auto& ps : props) {
        WritePropertyState(pkt, ps);
    }
    return pkt;
}

// Helper to read one PropertyState from packet
static PropertyState ReadPropertyState(Packet& pkt) {
    PropertyState ps;
    ps.objectId = pkt.ReadUInt();
    ps.flags    = pkt.ReadUInt();

    if (ps.flags & PR_POSITION) {
        ps.position = pkt.ReadVector3();
    }
    if (ps.flags & PR_ORIENTATION) {
        ps.orientation = pkt.ReadVector3();
    }
    if (ps.flags & PR_VELOCITY) {
        ps.velocity = pkt.ReadVector3();
    }
    if (ps.flags & PR_HEALTH) {
        ps.health = pkt.ReadInt();
    }
    if (ps.flags & PR_STATE) {
        ps.state = pkt.ReadUInt();
    }
    if (ps.flags & PR_CUSTOM) {
        uint32_t len = pkt.ReadUInt();
        ps.customData = pkt.ReadBytes(len);
    }
    return ps;
}

std::vector<PropertyState> PropertyReplication::ParsePacket(const Packet& pkt) {
    // Make mutable copy for reading
    Packet reader = pkt;
    uint32_t count = reader.ReadUInt();
    std::vector<PropertyState> list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        list.push_back(ReadPropertyState(reader));
    }
    return list;
}