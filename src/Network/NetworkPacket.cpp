// src/Network/NetworkPacket.cpp

#include "Network/NetworkPacket.h"
#include "Math/Vector3.h"
#include <cstring>
#include <chrono>

Packet::Packet(const std::string& tag)
    : m_tag(tag)
{}

Packet::Packet(const std::string& tag, const std::vector<uint8_t>& payload)
    : m_tag(tag), m_payload(payload)
{}

std::vector<uint8_t> Packet::Serialize() const {
    // Format: [4B tag length][tag bytes][4B payload length][payload bytes]
    std::vector<uint8_t> buf;
    uint32_t tagLen = (uint32_t)m_tag.size();
    uint32_t payloadLen = (uint32_t)m_payload.size();

    buf.resize(8 + tagLen + payloadLen);
    size_t offset = 0;
    std::memcpy(&buf[offset], &tagLen, 4); offset +=4;
    std::memcpy(&buf[offset], m_tag.data(), tagLen); offset += tagLen;
    std::memcpy(&buf[offset], &payloadLen, 4); offset +=4;
    if (payloadLen) {
        std::memcpy(&buf[offset], m_payload.data(), payloadLen);
    }
    return buf;
}

Packet Packet::FromBuffer(const std::vector<uint8_t>& buffer, PacketMetadata& meta) {
    Packet pkt;
    size_t offset=0;
    uint32_t tagLen=0;
    std::memcpy(&tagLen, &buffer[offset],4); offset+=4;
    pkt.m_tag.assign((char*)&buffer[offset], tagLen); offset+=tagLen;
    uint32_t payloadLen=0;
    std::memcpy(&payloadLen, &buffer[offset],4); offset+=4;
    if (payloadLen) {
        pkt.m_payload.assign(buffer.begin()+offset, buffer.begin()+offset+payloadLen);
    }
    // fill metadata
    meta.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    meta.rawTag = pkt.m_tag;
    pkt.m_readOffset = 0;
    return pkt;
}

const std::string& Packet::GetTag() const { return m_tag; }
uint32_t Packet::GetPayloadSize() const { return (uint32_t)m_payload.size(); }
const std::vector<uint8_t>& Packet::RawData() const { return m_payload; }

void Packet::WriteUInt(uint32_t v) {
    uint8_t buf[4]; std::memcpy(buf,&v,4);
    m_payload.insert(m_payload.end(), buf, buf+4);
}
void Packet::WriteInt(int32_t v)   { WriteUInt((uint32_t)v); }
void Packet::WriteFloat(float f) {
    uint8_t buf[4]; std::memcpy(buf,&f,4);
    m_payload.insert(m_payload.end(), buf, buf+4);
}
void Packet::WriteString(const std::string& s) {
    auto enc = EncodeString(s);
    m_payload.insert(m_payload.end(), enc.begin(), enc.end());
}
void Packet::WriteVector3(const Vector3& v) {
    WriteFloat(v.x); WriteFloat(v.y); WriteFloat(v.z);
}
void Packet::WriteBytes(const std::vector<uint8_t>& data) {
    m_payload.insert(m_payload.end(), data.begin(), data.end());
}

uint32_t Packet::ReadUInt() {
    uint32_t v=0;
    std::memcpy(&v, &m_payload[m_readOffset],4);
    m_readOffset+=4; return v;
}
int32_t Packet::ReadInt()    { return (int32_t)ReadUInt(); }
float Packet::ReadFloat() {
    float f=0; std::memcpy(&f, &m_payload[m_readOffset],4);
    m_readOffset+=4; return f;
}
std::string Packet::ReadString() {
    return DecodeString(m_payload, m_readOffset);
}
Vector3 Packet::ReadVector3() {
    Vector3 v; v.x=ReadFloat(); v.y=ReadFloat(); v.z=ReadFloat(); return v;
}
std::vector<uint8_t> Packet::ReadBytes(size_t count) {
    std::vector<uint8_t> out(m_payload.begin()+m_readOffset,
                              m_payload.begin()+m_readOffset+count);
    m_readOffset+=count; return out;
}

std::vector<uint8_t> Packet::EncodeString(const std::string& s) {
    std::vector<uint8_t> buf;
    uint32_t len=(uint32_t)s.size();
    buf.resize(4+len);
    std::memcpy(buf.data(), &len,4);
    std::memcpy(buf.data()+4, s.data(), len);
    return buf;
}

std::string Packet::DecodeString(const std::vector<uint8_t>& buf, size_t& offset) {
    uint32_t len=0;
    std::memcpy(&len, &buf[offset],4);
    offset+=4;
    std::string s((char*)&buf[offset], len);
    offset+=len;
    return s;
}